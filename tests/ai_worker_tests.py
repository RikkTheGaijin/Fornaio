"""Worker unit tests; no model weights or GPU are needed.

Run with a maintainer Python containing NumPy and Pillow. The application never
installs dependencies through this script.
"""
import importlib.util
import json
import re
import pathlib
import tempfile
import unittest
import numpy as np

from PIL import Image, ImageDraw

spec = importlib.util.spec_from_file_location(
    "fornaio_ai_worker", pathlib.Path(__file__).parents[1] / "scripts/ai_worker.py")
worker = importlib.util.module_from_spec(spec)
spec.loader.exec_module(worker)


def save(root, name, image):
    path = str(pathlib.Path(root) / (name + ".png"))
    image.save(path)
    return path


def facing_square(size=32):
    """Camera-space normals facing the camera inside a centered square, white background outside."""
    normal = Image.new("RGB", (size, size), "white")
    ImageDraw.Draw(normal).rectangle((8, 8, size - 9, size - 9), fill=(128, 128, 255))
    silhouette = Image.new("L", (size, size), 0)
    ImageDraw.Draw(silhouette).rectangle((8, 8, size - 9, size - 9), fill=255)
    return normal, silhouette


class GuideTests(unittest.TestCase):
    def test_clay_is_mid_gray_and_never_background_white(self):
        normal, silhouette = facing_square()
        clay = worker.clay(normal, silhouette)
        value = clay.getpixel((16, 16))
        self.assertEqual(value[0], value[2])  # Achromatic: the guide carries shape, not color.
        self.assertLess(value[0], 170)        # Near-white clay was left unpainted as background.
        self.assertEqual(clay.getpixel((2, 2)), (255, 255, 255))
        grazing = Image.new("RGB", (32, 32), (255, 128, 128))  # Normal perpendicular to the view.
        self.assertLess(worker.clay(grazing, Image.new("L", (32, 32), 255)).getpixel((5, 5))[0], value[0])

    def test_continuation_keeps_paint_and_shows_clay_elsewhere(self):
        with tempfile.TemporaryDirectory() as root:
            normal, silhouette = facing_square()
            feedback = Image.new("RGB", (32, 32), "white")
            ImageDraw.Draw(feedback).rectangle((8, 8, 23, 23), fill=(150, 150, 150))
            ImageDraw.Draw(feedback).rectangle((8, 8, 15, 23), fill=(200, 30, 20))
            mask = Image.new("L", (32, 32), 0)
            ImageDraw.Draw(mask).rectangle((8, 8, 15, 23), fill=255)
            command = dict(conditioning="confidence", normal=save(root, "normal", normal),
                           silhouette=save(root, "silhouette", silhouette),
                           feedback=save(root, "feedback", feedback), feedbackMask=save(root, "mask", mask))
            guide = worker.guide_image(command)
            self.assertEqual(guide.getpixel((10, 16)), (200, 30, 20))
            clay = worker.clay(normal, silhouette).getpixel((20, 16))
            self.assertEqual(guide.getpixel((20, 16)), clay)
            self.assertEqual(guide.getpixel((2, 2)), (255, 255, 255))
            fresh = worker.guide_image(dict(command, conditioning="fresh"))
            self.assertEqual(fresh.getpixel((10, 16)), clay)  # Fresh views never see old paint.

    def test_references_drop_hidden_color_and_respect_orientation(self):
        with tempfile.TemporaryDirectory() as root:
            reference = Image.new("RGBA", (64, 32), (255, 0, 255, 0))
            ImageDraw.Draw(reference).rectangle((0, 0, 31, 31), fill=(20, 60, 170, 255))
            oriented = Image.new("RGB", (64, 32), (20, 60, 170))
            exif = Image.Exif()
            exif[274] = 6
            oriented.save(pathlib.Path(root) / "oriented.jpg", exif=exif)
            paths = [save(root, "reference", reference), str(pathlib.Path(root) / "oriented.jpg")]
            images = worker.load_references(dict(references=paths + paths))
            self.assertEqual(len(images), worker.MAX_REFERENCES)
            self.assertEqual(images[0].getpixel((8, 16)), (20, 60, 170))
            self.assertEqual(images[0].getpixel((56, 16)), (255, 255, 255))
            self.assertEqual(images[1].size, (32, 64))


class CaptionTests(unittest.TestCase):
    def test_view_is_named_relative_to_the_front(self):
        self.assertEqual(worker.view_description({}), "identical to <image_1>")
        self.assertTrue(worker.view_description(dict(viewTurn=10)).startswith("front view"))
        self.assertTrue(worker.view_description(dict(viewTurn=90)).startswith("side view"))
        self.assertTrue(worker.view_description(dict(viewTurn=0, viewPitch=70)).startswith("top-down"))

    def test_rear_view_assumes_nothing_about_the_object(self):
        rear = worker.view_description(dict(viewTurn=180))
        self.assertTrue(rear.startswith("rear view"))
        for word in ("face", "eye", "nose", "mouth", "head"):
            self.assertNotIn(word, rear)

    def test_caption_carries_only_rules_plus_the_prompt_and_reference_descriptions(self):
        described = ["A blue faceted sphere with a black lightning bolt.", "Green moss on gray stone."]
        fresh = json.loads(worker.generation_caption(dict(prompt="", viewTurn=180), described))
        self.assertIn("no shadows", fresh["lighting"]["shadows"])
        self.assertIn("<image_2> and <image_3>", fresh["edit_instruction"])
        self.assertIn("keeps its exact shape", fresh["edit_instruction"])
        details = fresh["objects"][0]["appearance_details"]
        self.assertIn("<image_2>: A blue faceted sphere", details)
        self.assertIn("<image_3>: Green moss", details)
        self.assertTrue(fresh["photographic_characteristics"]["camera_angle"].startswith("rear view"))
        self.assertEqual(fresh["text_render"], [])
        template = json.dumps(json.loads(worker.generation_caption(dict(prompt="", viewTurn=180), [""]))).lower()
        for assumption in ("wear", "stains?", "rust", "weathered", "material shown", "faces?", "eyes?"):
            # Appearance comes only from the prompt and the references, never from the fixed template.
            self.assertIsNone(re.search(rf"\b{assumption}\b", template), assumption)
        prompted = json.loads(worker.generation_caption(dict(prompt="Red lacquer."), []))
        self.assertIn("Red lacquer", prompted["edit_instruction"])
        self.assertNotIn("<image_2>", json.dumps(prompted))
        later = json.loads(worker.generation_caption(dict(conditioning="confidence", prompt="Red lacquer"), []))
        self.assertIn("keep every painted area", later["edit_instruction"])


class BackgroundGuardTests(unittest.TestCase):
    def test_only_white_left_on_bare_clay_is_guarded(self):
        with tempfile.TemporaryDirectory() as root:
            _, silhouette = facing_square()
            mask = Image.new("L", (32, 32), 0)
            ImageDraw.Draw(mask).rectangle((8, 8, 15, 23), fill=255)  # Reused paint on the left half.
            command = dict(conditioning="confidence", silhouette=save(root, "silhouette", silhouette),
                           feedbackMask=save(root, "mask", mask))
            image = Image.new("RGB", (32, 32), "white")
            ImageDraw.Draw(image).rectangle((8, 8, 23, 15), fill=(120, 80, 40))
            guard = np.asarray(worker.background_guard(image, command))
            self.assertEqual(guard[20, 20], 0)    # Clay left white: excluded from projection.
            self.assertEqual(guard[20, 10], 255)  # White over reused paint stays (white paint).
            self.assertEqual(guard[10, 20], 255)  # Painted clay stays.
            self.assertIsNone(worker.background_guard(image, dict(command, conditioning="fresh")))


class TrustedDetailTests(unittest.TestCase):
    def test_cores_keep_one_sharp_detail_source_and_weak_areas_remain_free(self):
        with tempfile.TemporaryDirectory() as directory:
            accepted = Image.new("RGB", (64, 64), "red")
            ImageDraw.Draw(accepted).rectangle((12, 0, 14, 63), fill="black")
            generated = Image.new("RGB", (64, 64), "blue")
            ImageDraw.Draw(generated).rectangle((18, 0, 20, 63), fill="white")
            weights = np.zeros((64, 64), dtype=np.uint8)
            weights[:, :32] = 255
            weights[:, 32:48] = 80
            old_path = str(pathlib.Path(directory) / "accepted.png")
            mask_path = str(pathlib.Path(directory) / "confidence.png")
            accepted.save(old_path)
            Image.fromarray(weights).save(mask_path)
            result, kept = worker.preserve_trusted_details(generated, dict(
                conditioning="confidence", accepted=old_path, feedbackMask=mask_path, protectDetails=True))
            self.assertEqual(result.getpixel((13,32)), (0,0,0))
            self.assertEqual(result.getpixel((19,32)), (255,0,0))  # The conflicting new crack is absent.
            self.assertEqual(result.getpixel((40,32)), (0,0,255))
            self.assertEqual(result.getpixel((56,32)), (0,0,255))
            alpha = np.asarray(kept)
            self.assertEqual(alpha[32,13], 255)
            self.assertEqual(alpha[32,56], 0)
            self.assertLess(np.count_nonzero((alpha[32] > 0) & (alpha[32] < 255)), 10)

    def test_continuous_trusted_ramp(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            accepted = root / "accepted.png"
            mask = root / "mask.png"
            Image.new("RGB", (256,32), "red").save(accepted)
            Image.fromarray(np.tile(np.arange(256, dtype=np.uint8), (32,1))).save(mask)
            result, kept = worker.preserve_trusted_details(Image.new("RGB", (256,32), "blue"), dict(
                conditioning="confidence", continuityVersion=2, accepted=str(accepted),
                feedbackMask=str(mask), protectDetails=True))
            alpha = np.asarray(kept)[16].astype(int)
            self.assertEqual(alpha[0], 0)
            self.assertEqual(alpha[127], 0)
            self.assertEqual(alpha[255], 255)
            self.assertLessEqual(np.max(np.diff(alpha)), 4)
            self.assertTrue(np.all(np.diff(alpha) >= 0))
            self.assertEqual(result.getpixel((255,16)), (255,0,0))
            self.assertEqual(result.getpixel((64,16)), (0,0,255))

    def test_lock_can_be_disabled_without_reading_previous_images(self):
        image = Image.new("RGB", (32,32), "blue")
        for command in [dict(conditioning="fresh"), dict(conditioning="confidence", protectDetails=False)]:
            result, mask = worker.preserve_trusted_details(image, command)
            self.assertIs(result, image)
            self.assertIsNone(mask)

    def test_mismatched_context_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            old_path = str(pathlib.Path(directory) / "old.png")
            mask_path = str(pathlib.Path(directory) / "mask.png")
            Image.new("RGB", (16,16)).save(old_path)
            Image.new("L", (32,32), 255).save(mask_path)
            with self.assertRaises(ValueError):
                worker.preserve_trusted_details(Image.new("RGB", (32,32)), dict(
                    conditioning="confidence", accepted=old_path, feedbackMask=mask_path))


class GeometryDiagnosticsTests(unittest.TestCase):
    def test_original_source_survives_legacy_detail_restoration(self):
        with tempfile.TemporaryDirectory() as directory:
            generated = Image.new("RGB", (32, 32), "blue")
            accepted = save(directory, "accepted", Image.new("RGB", (32, 32), "red"))
            mask = save(directory, "confidence", Image.new("L", (32, 32), 255))
            output = str(pathlib.Path(directory) / "output.png")
            source, retained, _ = worker.save_generation_images(generated, dict(
                conditioning="confidence", accepted=accepted, feedbackMask=mask, silhouette=mask,
                protectDetails=True), output)
            with Image.open(source) as raw:
                self.assertEqual(raw.getpixel((16, 16)), (0, 0, 255))
            with Image.open(output) as processed:
                self.assertEqual(processed.getpixel((16, 16)), (255, 0, 0))
            self.assertTrue(pathlib.Path(retained).is_file())

    def test_exact_and_shifted_silhouettes(self):
        with tempfile.TemporaryDirectory() as directory:
            mask = Image.new("L", (64, 64), 0)
            ImageDraw.Draw(mask).rectangle((16, 16, 47, 47), fill=255)
            path = str(pathlib.Path(directory) / "mask.png")
            mask.save(path)
            exact = Image.new("RGB", (64, 64), "white")
            ImageDraw.Draw(exact).rectangle((16, 16, 47, 47), fill="red")
            self.assertEqual(worker.geometry_alignment(exact, path), 1.0)
            shifted = Image.new("RGB", (64, 64), "white")
            ImageDraw.Draw(shifted).rectangle((25, 16, 56, 47), fill="red")
            self.assertLess(worker.geometry_alignment(shifted, path), .75)

    def test_white_material_is_unscored(self):
        with tempfile.TemporaryDirectory() as directory:
            path = str(pathlib.Path(directory) / "mask.png")
            Image.new("L", (64, 64), 255).save(path)
            self.assertIsNone(worker.geometry_alignment(Image.new("RGB", (64, 64), "white"), path))

    def test_dimensions_and_missing_mask(self):
        image = Image.new("RGB", (64, 64), "red")
        self.assertIsNone(worker.geometry_alignment(image, None))
        with tempfile.TemporaryDirectory() as directory:
            path = str(pathlib.Path(directory) / "mask.png")
            Image.new("L", (32, 32), 255).save(path)
            with self.assertRaises(ValueError):
                worker.geometry_alignment(image, path)


if __name__ == "__main__":
    unittest.main()
