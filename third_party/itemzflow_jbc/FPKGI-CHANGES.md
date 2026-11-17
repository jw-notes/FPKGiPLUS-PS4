Based on the Jailbreak PRX from Itemzflow by LM, Sleirsgoevy and Specter:
https://github.com/LightningMods/Itemzflow
Upstream credits libjbc: https://github.com/sleirsgoevy/ps4-libjbc

Distributed under Itemzflow's GPL v3 licence; see LICENSE.

FPKGi+ changes: check unsupported kernel calls, pass all six variadic call
arguments, bound process-list traversal, include stdlib.h, and preserve const
qualifiers for mount arguments. OpenOrbis runtime module entry/exit points
replace the legacy definitions. The project's Makefile builds the helper from
this source using the shared OpenOrbis toolchain. No prebuilt Itemzflow binary
is used. The helper is loaded only as a fallback when GoldHEN's SDK is absent.
