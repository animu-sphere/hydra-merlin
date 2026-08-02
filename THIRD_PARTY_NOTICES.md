# Third-Party Notices

## Dear ImGui

`merlin-viewport` privately embeds Dear ImGui and its official GLFW/Vulkan
backends for the renderer-development diagnostic surface.

- Upstream repository: ocornut/imgui
- Upstream revision: `8936b58fe26e8c3da834b8f60b06511d537b4c63`
- Version at the pinned revision: 1.92.8
- Copyright: Copyright (c) 2014-2026 Omar Cornut
- License: MIT; viewport builds install the upstream `LICENSE.txt` under
  `share/merlin/licenses/imgui`

## MaterialX

The optional `Merlin::MaterialX` compiler links the official MaterialX Slang
Shader Generator. The source fallback is pinned to the upstream revision below;
compatible packaged builds may be supplied instead.

- Upstream repository: AcademySoftwareFoundation/MaterialX
- Upstream revision: `38368ee04da84ce1f8837ecba7322dd6d81291f8`
- Version at the pinned revision: 1.39.6
- Copyright: Copyright Contributors to the MaterialX Project
- License: Apache License 2.0; source builds install the upstream `LICENSE`
  under `share/merlin/licenses/materialx`

## Native File Dialog Extended

Hydra-enabled `merlin-viewport` builds privately embed Native File Dialog
Extended to invoke the platform file chooser from the renderer-development
diagnostic surface.

- Upstream repository: btzy/nativefiledialog-extended
- Upstream revision: `3cd252a8f7ca32419b1ca235c2990ba6a0ecba7c`
- Version at the pinned revision: 1.3.0
- Authors: Bernard Teo and Michael Labbe
- License: zlib; viewport builds install the upstream `LICENSE` under
  `share/merlin/licenses/nativefiledialog-extended`

## OpenUSD Stinson Beach environment image

`backend/merlin-vulkan/assets/environment.hdr` is an unmodified copy of
`StinsonBeach.hdr` from OpenUSD v26.05, renamed for use as Merlin's default
environment image.

- Upstream repository: OpenUSD
- Upstream revision: `2095fafafd033fa23386d7ec6d58c7cc33974518`
- Upstream path: `pxr/imaging/hdx/textures/StinsonBeach.hdr`
- SHA-256: `4897697c757edc524dc9b7bcc692e8e05a7f02dbede3e30d2291dc0831dece17`
- Copyright: Copyright 2016 Pixar. All rights reserved.
- License: Tomorrow Open Source Technology License 1.0, reproduced in
  `third_party/openusd/LICENSE.txt`
- Upstream notice: `third_party/openusd/NOTICE.txt`
