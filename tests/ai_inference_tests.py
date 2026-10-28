"""Worker numerics with the optional runtime's PyTorch; no model weights are needed."""
import importlib.util
import pathlib
import types
import unittest

import torch

spec = importlib.util.spec_from_file_location(
    "fornaio_ai_worker", pathlib.Path(__file__).parents[1] / "scripts/ai_worker.py")
worker = importlib.util.module_from_spec(spec)
spec.loader.exec_module(worker)


class PatchEmbedTests(unittest.TestCase):
    def test_linear_patchify_matches_the_strided_conv3d(self):
        torch.manual_seed(0)
        conv = torch.nn.Conv3d(3, 8, kernel_size=(2, 4, 4), stride=(2, 4, 4), bias=True)
        patch_embed = types.SimpleNamespace(proj=conv, forward=None)
        patches = torch.randn(5, 3 * 2 * 4 * 4)
        expected = conv(patches.view(-1, 3, 2, 4, 4)).view(-1, 8)
        worker.linear_patch_embed(patch_embed)
        torch.testing.assert_close(patch_embed.forward(patches), expected, rtol=1e-5, atol=1e-5)


@unittest.skipUnless(torch.cuda.is_available() and torch.cuda.get_device_capability() >= (8, 9),
                     "FP8 needs an Ada or Hopper GPU")
class Fp8Tests(unittest.TestCase):
    def test_quantized_linear_tracks_bfloat16_and_follows_device_moves(self):
        torch.manual_seed(0)
        model = torch.nn.Sequential(torch.nn.Linear(1024, 2048), torch.nn.Linear(2048, 16)).to(torch.bfloat16)
        x = torch.randn(64, 1024, dtype=torch.bfloat16)
        expected = model.cuda()(x.cuda()).float()
        model.cpu()
        worker.quantize_fp8(model, torch.device("cuda"))
        self.assertEqual(type(model[0]).__name__, "Fp8Linear")
        self.assertIsInstance(model[1], torch.nn.Linear)  # Small layers stay in bfloat16.
        self.assertEqual(model[0].weight_fp8.device.type, "cpu")  # Returned to its device for offloading.
        actual = model.cuda()(x.cuda()).float()
        error = (actual - expected).norm() / expected.norm()
        self.assertLess(error.item(), .05)


if __name__ == "__main__":
    unittest.main()
