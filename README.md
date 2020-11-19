# Extended Falcor 4.2

This repository is based on [Falcor 4.2](https://github.com/NVIDIAGameWorks/Falcor/releases/tag/4.2), origin document can be found [here](Docs/Readme.md).

Document of some features can be found [here](Docs/Usage/Extended-Falcor.md).

## Features
 - Support comment in `fscene` file [#e45efe7](https://github.com/VicentChen/Falcor/commit/e45efe7210cdb265c730dfa27ddd341d66505543).
 - Support hit group type `D3D12_HIT_GROUP_TYPE_PROCEDURAL_PRIMITIVE` [#2798308](https://github.com/VicentChen/Falcor/commit/2798308aafcd5ffdaa5bcc0fa4f4f0439fcc8299).
 - Support procedural scene [#4b2437a](https://github.com/VicentChen/Falcor/commit/4b2437aa9325a2f50d3abfd1f812adf8dfa56b8a).
 - Support model material in `fscene` [#a0c932f](https://github.com/VicentChen/Falcor/commit/a0c932f5f03270f1a052d6073a5260cabac9d084)

## Fixes
 - Encoding error in `Source/Samples/make_new_project.py` [#c547d6a](https://github.com/VicentChen/Falcor/commit/c547d6a1beb81c8dbdfb76d8398eab4c8fc9891f).
 - Support more models [#d93a4f](https://github.com/VicentChen/Falcor/commit/d93a4f99e4b8d677719c25a9850b26e88cd32e02).
 - Extend number of instance matrix( or `drawCount`) limitation to `2^20 - 1` (temporary solution, while maximum number of meshes reduced to `2^11 - 1` ) [#337246a](https://github.com/VicentChen/Falcor/commit/337246abcf80f0fc8550649ab7eced1adc043167).
 - Any hit shader not invoked for object with base color material [#cb4ba5c](https://github.com/VicentChen/Falcor/commit/cb4ba5c1191f111f84b89a51cc6d6a49a85a3576)

## TODOs
 - Extend mesh and instance limitation from 32 bits to 64 bits.
 - Add preview support to preview bounding boxes.
 - Refactor `Scene`, `ProceduralScene` to remove redundant code.
 - Support model material in `pyscene`.
