ifeq (,$(wildcard config.mak))
MAKE=make
else
include config.mak
endif
include VERSION

DOXYGEN =

ifeq ($(DOC),yes)
  DOXYGEN = doxygen
endif

all: $(DOXYGEN)
	$(MAKE) -C libfosfat
	$(MAKE) -C libfosgra
	$(MAKE) -C tools
	$(MAKE) -C fosmount

doxygen:
ifeq (,$(wildcard DOCS/doxygen))
	 PROJECT_NUMBER="${VERSION}.${PATCHLEVEL}.${SUBLEVEL}" doxygen DOCS/Doxyfile
endif

clean:
	if [ -f config.win32 ]; then \
	  $(MAKE) -C libw32disk clean; \
	fi
	if [ -f libfosfat/Makefile ]; then \
	  $(MAKE) -C libfosfat clean; \
	fi
	if [ -f libfosgra/Makefile ]; then \
	  $(MAKE) -C libfosgra clean; \
        fi
	if [ -f tools/Makefile ]; then \
	  $(MAKE) -C tools clean; \
	fi
	if [ -f config.mak ]; then \
	  $(MAKE) -C fosmount clean; \
	fi

distclean: clean
	rm -f config.log
	rm -f config.mak
	rm -f config.win32
	rm -f libfosfat/Makefile
	rm -f libfosgra/Makefile
	rm -f tools/Makefile
	rm -rf DOCS/doxygen

install: install-deb install-dev

install-deb:
	$(MAKE) -C tools install
	$(MAKE) -C fosmount install

install-lib:
	$(MAKE) -C libfosfat install-lib
	$(MAKE) -C libfosgra install-lib

install-dev:
	$(MAKE) -C libfosfat install
	$(MAKE) -C libfosgra install

uninstall:
	cp -f libfosfat/Makefile.linux libfosfat/Makefile
	cp -f libfosgra/Makefile.linux libfosgra/Makefile
	cp -f tools/Makefile.linux tools/Makefile
	$(MAKE) -C libfosfat uninstall
	$(MAKE) -C libfosgra uninstall
	$(MAKE) -C tools uninstall
	$(MAKE) -C fosmount uninstall

win32-dev:
	./win32-gen.sh

win32-build:
	./win32-gen.sh build

win32-common:
	cp -f libfosfat/Makefile.win32 libfosfat/Makefile
	cp -f tools/Makefile.win32 tools/Makefile
	$(MAKE) -C libw32disk
	$(MAKE) -C libfosfat
	$(MAKE) -C tools

win32: win32-dev win32-common

win32-zip: win32-build win32-common

.PHONY: clean install-deb install-dev uninstall win32-dev win32-build win32-common
.PHONY: doxygen
