# Extended Falcor 4.2

This repository is based on [Falcor 4.2](https://github.com/NVIDIAGameWorks/Falcor/releases/tag/4.2), origin document can be found [here](Docs/Readme.md).

## Features
 - Support comment in `fscene` file [#e45efe7](https://github.com/VicentChen/Falcor/commit/e45efe7210cdb265c730dfa27ddd341d66505543)

## Fixes
 - Encoding error in `Source/Samples/make_new_project.py` [#c547d6a](https://github.com/VicentChen/Falcor/commit/c547d6a1beb81c8dbdfb76d8398eab4c8fc9891f)
 - Support more models [#d93a4f](https://github.com/VicentChen/Falcor/commit/d93a4f99e4b8d677719c25a9850b26e88cd32e02)
 - Extend number of instance matrix( or `drawCount`) limitation to `2^20 - 1` (temporary solution, while maximum number of meshes reduced to `2^11 - 1` ) [#337246a](https://github.com/VicentChen/Falcor/commit/337246abcf80f0fc8550649ab7eced1adc043167)