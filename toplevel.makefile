
.DEFAULT_GOAL = all

include $(TREE_PATH)/rules.makefile
$(call include_dir,frigg)
$(call include_dir,eir)
$(call include_dir,thor/kernel)
$(call include_dir,ld-init/linker)
#$(call include_dir,libnet)
$(call include_dir,drivers/libcompose)
$(call include_dir,drivers/libterminal)
$(call include_dir,drivers/vga_terminal)
#$(call include_dir,drivers/bochs_vga)
#$(call include_dir,drivers/ata)

$(call include_dir,tools/frigg_pb)
$(call include_dir,hel)

.PHONY: all clean gen

