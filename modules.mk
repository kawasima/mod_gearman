mod_gearman.la: mod_gearman.slo
	$(SH_LINK) -rpath $(libexecdir) -lgearman -module -avoid-version  mod_gearman.lo
DISTCLEAN_TARGETS = modules.mk
shared =  mod_gearman.la
