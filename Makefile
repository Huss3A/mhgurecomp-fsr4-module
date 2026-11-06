# FSR 4 engine module for MHGU Recomp (Windows x64, MSYS2 UCRT64: clang, make, curl).
#   make            builds build/fsr4/ : fsr4.dll + AMD's FSR SDK DLLs + licence texts
#   make install MODULES=<the port's engine_modules folder>    copies it there and adds `fsr4` to enabled.txt
# AMD's DLLs are not in this repository: they are downloaded from AMD's tagged SDK release and checked against their SHA-256.
CC       := clang
SDK_VER  := 2.4.0
SDK_URL  := https://raw.githubusercontent.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/v$(SDK_VER)/Kits/FidelityFX
DL       := build/download
OUT      := build/fsr4

CFLAGS   := -O2 -Wall -Wno-unused-function -Ithird_party/ffx-api -I.
LDFLAGS  := -shared -fuse-ld=lld -static-libgcc -lversion

all: $(OUT)/fsr4.dll $(OUT)/amd_fidelityfx_loader_vk.dll $(OUT)/amd_fidelityfx_upscaler_vk.dll $(OUT)/AMD-FSR-SDK.txt $(OUT)/LICENSE.txt
	@echo "built $(OUT)"

$(OUT)/fsr4.dll: fsr4_module.c engine_module_api.h third_party/ffx_vk_portable.h
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) fsr4_module.c $(LDFLAGS) -o $@

# FETCH(file, url, sha256)
define FETCH
$(1):
	@mkdir -p $$(dir $$@)
	curl -fL --retry 3 -o $$@.part $(2)
	echo "$(3)  $$@.part" | sha256sum -c -
	mv -f $$@.part $$@
endef
$(eval $(call FETCH,$(DL)/amd_fidelityfx_loader_vk.dll,$(SDK_URL)/signedbin/amd_fidelityfx_loader_vk.dll,38a56a01fb3565c0757d9f5c1e4e69de4b65e5f2e5769b323b200dc1f142bab9))
$(eval $(call FETCH,$(DL)/amd_fidelityfx_upscaler_vk.dll,$(SDK_URL)/signedbin/amd_fidelityfx_upscaler_vk.dll,4a7e895af6e0b615d40a84d59eb8c2b1054ad91cdd0c739567940270d51bdab7))
$(eval $(call FETCH,$(DL)/license.md,$(SDK_URL)/docs/license.md,32357259b965d6a8001e9e508ba60eda6034027c5923a7d226ffbaff813c2bfb))

$(OUT)/amd_fidelityfx_%_vk.dll: $(DL)/amd_fidelityfx_%_vk.dll
	@mkdir -p $(OUT)
	cp $< $@

# AMD's licence text goes with the DLLs: its license.md lists the signed DLLs under the MIT licence, whose notice must stay with them.
$(OUT)/AMD-FSR-SDK.txt: $(DL)/license.md
	@mkdir -p $(OUT)
	@{ echo "This folder holds AMD's FSR SDK $(SDK_VER) DLLs, unmodified: amd_fidelityfx_loader_vk.dll and"; \
	   echo "amd_fidelityfx_upscaler_vk.dll (FSR 4 upscaling), from https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK"; \
	   echo "(tag v$(SDK_VER)). AMD's license file lists them under the MIT licence; it is reproduced below."; \
	   echo; cat $<; } > $@

$(OUT)/LICENSE.txt: LICENSE
	@mkdir -p $(OUT)
	cp $< $@

.PHONY: install clean
install: all
	@test -n "$(MODULES)" || { echo "usage: make install MODULES=<the port's engine_modules folder>"; exit 1; }
	mkdir -p "$(MODULES)/fsr4"
	cp $(OUT)/* "$(MODULES)/fsr4/"
	@touch "$(MODULES)/enabled.txt"
	@grep -qx fsr4 "$(MODULES)/enabled.txt" || echo fsr4 >> "$(MODULES)/enabled.txt"
	@echo "installed: $(MODULES)/fsr4, listed in $(MODULES)/enabled.txt"

clean:
	rm -rf build
