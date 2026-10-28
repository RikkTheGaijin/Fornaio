# Optional AI Texturing components

Fornaio's application/source packages contain no Python distribution, AI model weights,
PyTorch, or large inference dependencies. `manifest.json` contains release-pinned public
download locations, sizes, hashes, and versions, not model data.

The default image model is **GPT Image 2** (OpenAI), a cloud service. Fornaio calls it
through the Codex CLI signed in with the user's ChatGPT account (the plan's image limits
apply), or through the OpenAI Images API when `OPENAI_API_KEY` is set (billed per image).
For each camera view the guide render (mesh shape, already accepted paint, gray clay), the
first reference image and the prompt are uploaded to OpenAI. Generated images are subject
to OpenAI's terms: https://openai.com/policies/

The optional local engine uses **Fibo 2.0 Edit Turbo** (Bria) with its supplied FLUX.2
VAE, Qwen3-VL text encoder, processor and fonts. Fibo is gated on Hugging Face: accept its
license with your account and provide an access token for the download. It is licensed
**CC BY-NC 4.0** (non-commercial); commercial use needs a license from Bria. Model license
and attribution are downloaded alongside the weights.

- Model: https://huggingface.co/briaai/Fibo-2.0-edit-turbo
- CC BY-NC 4.0: https://creativecommons.org/licenses/by-nc/4.0/
- Bria licensing: https://platform.bria.ai/licensing/pricing?model=fibo-2
- Python embeddable distribution: PSF license and its incorporated-software notices.
- PyTorch: BSD-style license plus all notices supplied in its binary wheel.
- Diffusers, Transformers, Accelerate, Hugging Face Hub, Safetensors: Apache-2.0.
- Additional binary dependencies retain their individual `.dist-info` license files.
- The tokenizers wheel's missing Apache license is downloaded separately from its
  versioned upstream release and installed alongside the package.
- CUDA/cuDNN binaries in the official PyTorch archive retain NVIDIA's applicable
  licenses and distribution conditions; they are not covered by Apache-2.0.

Pinned component versions and provenance are recorded in `manifest.json`. Installation
extracts original release archives without removing license/notice files. The maintainer
must audit the exact resolved wheel licenses and NVIDIA redistributables before publishing
an assembled offline pack. Existing Qt LGPL and Fornaio third-party obligations continue
to apply independently.

Downloads contact the listed HTTPS distribution hosts; the Hugging Face token is sent to
huggingface.co only and is not stored. With the local engine, texture generation runs
locally and mesh renders, descriptions and references are not uploaded.
