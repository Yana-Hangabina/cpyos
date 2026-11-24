# Native plugin catalog

The device checks `https://yana-hangabina.github.io/cpyos/plugins/manifest-v1.txt`
by default. Put a card file at `/.crosspoint/plugins/catalog.txt` to use a local
catalog without changing firmware; it has the same format.

```
PEXT1
id	name	version	api	arch	size	sha256	url	description
2048	2048	1.0.0	1	riscv32	2808	<sha256>	https://…/2048-riscv32.elf	Pilot plugin
2048	2048	1.0.0	1	xtensa	2668	<sha256>	https://…/2048-xtensa.elf	Pilot plugin
```

Generate it from packaged ELFs:

```bash
python3 scripts/build_plugin_catalog.py --out plugins/catalog/manifest-v1.txt \
  --base-url https://yana-hangabina.github.io/cpyos/plugins \
  --artifact 2048:2048:1.0.0:riscv32:plugins/native/2048/dist/plugin.elf \
  --artifact 2048:2048:1.0.0:xtensa:plugins/native/2048/dist-xtensa/plugin.elf
```

Publish the manifest and the two ELFs at the same `--base-url` (Pages,
Release assets, or any HTTPS host). The installer replaces an existing package
only after size and SHA-256 pass; a malformed catalog or package is rejected
without touching the installed copy. `file://` URLs are accepted for local
catalog testing.
