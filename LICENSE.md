# Licence

This file holds three licences:

1. **The SonicWebMania licence** (below), for this port's own code.
2. **The RSDKv5(U) Decompilation licence**, for the engine this port builds on.
3. **The Sonic Mania Decompilation licence**, for the game code this port builds on.

All three apply to this repository and to every build made from it.
Third-party libraries (SDL2, cpp-httplib, libogg, ...) keep their own licences,
listed in `THIRD_PARTY_NOTICES.md` with their full texts in `licenses/`.

---

# SONICWEBMANIA SOURCE CODE LICENSE

Copyright (c) 2026 SonicWebMania contributors
https://github.com/pustal/SonicWebMania

SonicWebMania is a port of Sonic Mania to LG webOS TVs. It is built on two
decompilation projects that are not ours, by Rubberduckycooly and chuliRMG
(original RSDK by Evening Star):

- RSDKv5 Decompilation: https://github.com/RSDKModding/RSDKv5-Decompilation
- Sonic Mania Decompilation: https://github.com/RSDKModding/Sonic-Mania-Decompilation

It uses them through their Plus forks, which are under the same licences:

- RSDKv5-Decompilation-Plus: https://github.com/pustal/RSDKv5-Decompilation-Plus
- Sonic-Mania-Decompilation-Plus: https://github.com/pustal/Sonic-Mania-Decompilation-Plus

This licence covers the port's own work: the files in `src/`, `web/`,
`cmake/`, `scripts/`, `tools/` and `webos/`, and the build files and
documentation at the top of the repository. It follows the decompilations'
licences below, so the whole project can be shared under one set of terms.

This code is provided as-is, that is to say, without liability or warranty.
The authors of SonicWebMania are not held responsible for any damages or
other claims arising from its use.

You may copy, modify, contribute, and distribute it, for public or private
use, **as long as the following are followed:**

- You may not use it for commercial (any sort of profit) use: no paid builds,
  and no donations tied to builds.
- Builds you give to anyone, private or otherwise, must have the Sonic Mania
  Plus content off by default (this port keeps it off until the user opts in
  on the transfer page).
- You must credit SonicWebMania and, as their licences ask, the developers of
  the decompilations and of the original code, and make clear the
  decompilations are not yours.
- You may not distribute the game's assets: users supply their own
  `Data.rsdk`.
- This licence, with the two below, must be in all modified copies of the
  source code, and all forks must follow it.

---

# RSDKv5(U) DECOMPILATION SOURCE CODE LICENSE v2.1

The code in this repository is a decompilation of RSDK (Retro-Engine) version 5 and 5-Ultimate.
There is original code in this repo, but most of the code is to be functionally the same as the version of RSDK this repo specifies.

This code is provided as-is, that is to say, without liability or warranty. 
Original authors of RSDK and authors of the decompilation are not held responsible for any damages or other claims said.

You may copy, modify, contribute, and distribute, for public or private use, **as long as the following are followed:**
- All pre-built executables provided TO ANYONE *PRIVATE OR OTHERWISE* **must be built with DLC disabled by __default.__**
  - DLC is managed by the DummyCore Usercore. A define, `RSDK_AUTOBUILD`, and a CMake flag, `RETRO_DISABLE_PLUS`, are already provided for you to force DLC off.
  - Creating a configuration setting *is allowed,* so long as it is set to off by default.
    - *No such configuration will be pushed to the master repository.*
  - This is to ensure an extra layer of legal protection for Sonic Mania Plus and Sonic Origins Plus.
- You may not use the decompilation for commercial (any sort of profit) use.
- You must clearly specify that the decompilation and original code are not yours: the developers of both must be credited.
- You may not distribute assets used to run any game not directly provided by the repository (other than unique, modded assets).
- This license must be in all modified copies of source code, and all forks must follow this license.

Original RSDK authors: Evening Star

Decompilation authors: Rubberduckycooly and chuliRMG


---

# SONIC MANIA DECOMPILATION SOURCE CODE LICENSE v1

The code in this repository is a decompilation of Sonic Mania.
There is original code in this repo, but most of the code is expected to be functionally the same.

This code is provided as-is, that is to say, without liability or warranty. 
Original authors of RSDK and authors of the decompilation are not held responsible for any damages or other claims said.

You may copy, modify, contribute, and distribute, for public or private use, **as long as the following are followed:**
- You may not use the decompilation for commercial (any sort of profit) use.
- You must clearly specify that the decompilation and original code are not yours.
- You may not distribute assets used to run the game not directly provided by the repository (other than unique, modded assets).
- This license must be in all modified copies of source code, and all forks must follow this license.
