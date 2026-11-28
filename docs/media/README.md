# Open Chess Set in Garden

The [README demo](merlin-openchess-garden.gif) places the MaterialX-shaded Open
Chess Set on the table in a 3D Gaussian reconstruction of Garden.

## Capture

- OpenUSD 26.08 `usdrecord` on Windows, using the hdMerlin Hydra 2 adapter and
  Vulkan backend. The exact hdMerlin build commit is unknown.
- The scene positions the chess set on the garden table, adds a contact shadow
  with Gaussian splats, and lights the chess set with a dome and distant
  lights. The `gaussian-ply` plugin imports the garden as a
  `ParticleField3DGaussianSplat`.
- 1280 × 720 capture frames, encoded as a 704 × 396, six-second looping GIF.
  The README edition starts on the wide shot and uses a smaller color palette
  to reduce the download size.

## Asset credits

- **Open Chess Set** by Moeen and Mujtaba Sayed, contributed by SideFX.
  The Academy Software Foundation licenses the USD reference asset under
  [CC BY 4.0](https://creativecommons.org/licenses/by/4.0/).
  [Upstream asset and adaptation credits](https://github.com/usd-wg/assets/tree/main/full_assets/OpenChessSet)
- **Garden** from Mip-NeRF 360 by Jonathan T. Barron, Ben Mildenhall, Dor
  Verbin, Pratul P. Srinivasan, and Peter Hedman (CVPR 2022). The demo uses
  the msplat-trained PLY distributed by `alexmkwizu`. The SMERF project lists
  the source dataset under [CC BY 4.0](https://creativecommons.org/licenses/by/4.0/).
  [Original dataset](https://jonbarron.info/mipnerf360/) · [Dataset terms](https://smerf-3d.github.io/) · [PLY source](https://huggingface.co/datasets/alexmkwizu/gaussian_training_datasets/tree/main/tested_outputs)
- **Lush Dirt Path** by Alexander Scholten, distributed by Poly Haven under
  CC0. [HDRI source](https://polyhaven.com/a/lush_dirt_path)

This directory contains the rendered preview. The source assets are available
from their respective upstream projects.
