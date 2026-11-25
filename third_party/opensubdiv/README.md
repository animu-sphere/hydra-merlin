# OpenSubdiv

The `upstream/` directory contains the unmodified Far, Sdc and Vtr C++
sources and headers, version header and license notices from OpenSubdiv
`v3_6_1`. These files build into `Merlin::RenderWorld` when
`MERLIN_ENABLE_OPENSUBDIV=ON`. The build uses the checked-in sources offline.

To update the snapshot, take the same paths from the chosen upstream tag,
update the CMake source list and run the native subdivision and Hydra fixture
tests. The source archive for this snapshot has SHA-256
`e9d99a480d80e999667643747c76eb0138d36c418fc154fd71b4bd65be103d52`.
[OpenSubdiv v3_6_1](https://github.com/PixarAnimationStudios/OpenSubdiv/releases/tag/v3_6_1)
