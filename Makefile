ROOT_DIR ?= $(abspath ..)
include $(ROOT_DIR)/mk/toolchain.mk

VERSION := $(shell cat VERSION)
PKGREL ?= 1
OUT := $(BIN_DIR)/video-backend
OBJ := $(OUT)/obj
LIB_DIR := $(BIN_DIR)/lib
LIBPUREC := $(LIB_DIR)/libpurec.a
CRT0 := $(LIB_DIR)/crt0.o
LINKER_SCRIPT := $(ROOT_DIR)/linker-userspace.ld

CFLAGS := $(CPPFLAGS) -I$(ROOT_DIR)/src/libc/include/hosted -I$(CURDIR)/include $(USER_CFLAGS) -O2 -Wall -Wextra $(DEPFLAGS)

PROGRAMS := $(OUT)/videod $(OUT)/vb-demo
LIBVB := $(OUT)/libvb.a

.PHONY: all clean package
all: $(PROGRAMS) $(LIBVB)

$(OBJ)/%.o: %.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c $< -o $@

$(LIBVB): $(OBJ)/client/vb.o
	@mkdir -p $(@D)
	rm -f $@
	$(AR) rcs $@ $^

$(OUT)/videod: $(OBJ)/server/videod.o $(CRT0) $(LIBPUREC) $(LINKER_SCRIPT)
	$(LD) -T $(LINKER_SCRIPT) -o $@ $(CRT0) $(OBJ)/server/videod.o $(LIBPUREC)

$(OUT)/vb-demo: $(OBJ)/demo/vb-demo.o $(LIBVB) $(CRT0) $(LIBPUREC) $(LINKER_SCRIPT)
	$(LD) -T $(LINKER_SCRIPT) -o $@ $(CRT0) $(OBJ)/demo/vb-demo.o $(LIBVB) $(LIBPUREC)

$(CRT0) $(LIBPUREC):
	$(MAKE) -C $(ROOT_DIR)/src/libc

PKG_FILE := $(OUT)/video-backend-$(VERSION)-$(PKGREL)-x86_64.pkg.tar.gz
SIGN_KEY ?= $(HOME)/.config/purec-pkg/signing.pem

package: all
	rm -f $(OUT)/video-backend-*.pkg.tar.gz*
	python3 $(ROOT_DIR)/mk/pkg/mkpkg.py --name video-backend --version $(VERSION)-$(PKGREL) \
		--desc "Display server (videod) and its client library" -o $(OUT) \
		--file usr/bin/videod=$(OUT)/videod \
		--file usr/bin/vb-demo=$(OUT)/vb-demo \
		--file usr/lib/libvb.a=$(LIBVB) \
		--file usr/include/vb.h=$(CURDIR)/include/vb.h \
		--file usr/include/vb_proto.h=$(CURDIR)/include/vb_proto.h \
		--file usr/lib/init/system/videod.service=$(CURDIR)/units/videod.service \
		--file usr/lib/init/system/graphical.target.wants/videod.service=$(CURDIR)/units/videod.service
	if [ -f "$(SIGN_KEY)" ]; then python3 $(ROOT_DIR)/mk/pkg/pkgsign.py sign $(SIGN_KEY) $(PKG_FILE) $(PKG_FILE).sig; fi
	@ls -l $(OUT)/video-backend-*

clean:
	rm -rf $(OUT)

-include $(shell find $(OBJ) -name '*.d' 2>/dev/null)
