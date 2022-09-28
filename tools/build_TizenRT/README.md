# Build test on TizenRT

You should setup the environment to build TizenRT.
See [TizenRT](https://github.com/Samsung/TizenRT) for the details.


## How to run build-test

```
TizenRT
  -- build
    -- configs
      -- artik053
        -- st_things
          -- defconfig  << add config (CONFIG_NNSTREAMER_EDGE=y)
        -- tc
          -- defconfig  << add config (CONFIG_NNSTREAMER_EDGE=y)
      -- artik055s
        -- audio
          -- defconfig  << add config (CONFIG_NNSTREAMER_EDGE=y)
      (extra config for each build target)
  -- external
    -- nnstreamer-edge  << clone nnstreamer-edge sources under 'external' directory
  -- os
    -- tools
      -- build_test.sh  << update build_targets
```


Below is an example to run build-test on TizenRT.

1. Download sources, prepare build-test.

```
# clone TizenRT and nnstreamer-edge
$ git clone https://github.com/Samsung/TizenRT.git
$ git clone https://github.com/nnstreamer/nnstreamer-edge.git ./TizenRT/external/nnstreamer-edge

# copy make file to nnstreamer-edge directory
$ cp ./TizenRT/external/nnstreamer-edge/tools/build_TizenRT/* ./TizenRT/external/nnstreamer-edge
```

2. Set build target, update configuration and related sources before running build-test.

    *NOTE: To get the build logs, block rm command at the end of the script 'build_test.sh'.

```
# Update build_targets (TizenRT/os/tools/build_test.sh).
build_targets=(
	"artik055s/audio"
	"artik053/st_things"
	"artik053/tc"
	"qemu/build_test"
	"imxrt1020-evk/loadable_elf_apps"
	"imxrt1050-evk/loadable_elf_apps"
	"rtl8721csm/hello"
	"rtl8721csm/loadable_apps"
	"rtl8721csm/tc"
)

# Add configuration (TizenRT/build/configs/<target>/defconfig).
CONFIG_NNSTREAMER_EDGE=y
```

3. Run build-test.
```
$ cd ./TizenRT/os
$ ./dbuild.sh menu
# Select 't' for build-test, you can find build-test result under 'TizenRT/os/tools/build_test' directory.
```
