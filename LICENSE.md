# Licence

SonicWebMania is a port of two decompilation projects that are not ours:

- **RSDKv5 Decompilation** by Rubberduckycooly and chuliRMG (original RSDK by Evening Star):
  https://github.com/RSDKModding/RSDKv5-Decompilation
- **Sonic Mania Decompilation** by the same team:
  https://github.com/RSDKModding/Sonic-Mania-Decompilation

The engine is used with a small patch (`patches/rsdk/`). Both licences below
apply to this repository and to every build made from it. In particular:

- no commercial use of any kind (no paid builds, no donations tied to builds);
- builds you give to anyone must have the Plus content off by default
  (this port keeps it off until the user opts in on the transfer page);
- no game assets are distributed: users supply their own `Data.rsdk`.

The port's own files (`src/`, `web/`, `cmake/`, `scripts/`, `webos/`) are
released under the same terms so the whole project can be shared as one.

Third-party components are listed in `THIRD_PARTY_NOTICES.md`.

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
