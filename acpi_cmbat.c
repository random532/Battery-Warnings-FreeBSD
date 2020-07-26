/*-
 * Copyright (c) 2005 Nate Lawson
 * Copyright (c) 2000 Munehiro Matsuda
 * Copyright (c) 2000 Takanori Watanabe
 * Copyright (c) 2000 Mitsuru IWASAKI <iwasaki@FreeBSD.org>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <sys/cdefs.h>
__FBSDID("$FreeBSD: releng/12.1/sys/dev/acpica/acpi_cmbat.c 336914 2018-07-30 15:46:40Z asomers $");

#include "opt_acpi.h"
#include <sys/param.h>
#include <sys/kernel.h>
#include <sys/module.h>
#include <sys/bus.h>
#include <sys/ioccom.h>

#include <machine/bus.h>
#include <sys/rman.h>
#include <sys/malloc.h>

#include <contrib/dev/acpica/include/acpi.h>

#include <dev/acpica/acpivar.h>
#include <dev/acpica/acpiio.h>

#include <sys/sysctl.h>

static MALLOC_DEFINE(M_ACPICMBAT, "acpicmbat",
    "ACPI control method battery data");

/* Number of times to retry initialization before giving up. */
#define ACPI_CMBAT_RETRY_MAX	6

/* Check the battery once a minute. */
#define	CMBAT_POLLRATE		(60 * hz)

/* Hooks for the ACPI CA debugging infrastructure */
#define	_COMPONENT	ACPI_BATTERY
ACPI_MODULE_NAME("BATTERY")

#define	ACPI_BATTERY_BST_CHANGE	0x80
#define	ACPI_BATTERY_BIF_CHANGE	0x81

struct acpi_cmbat_softc {
    device_t	    dev;
    int		    flags;

    struct acpi_bif bif;
    struct acpi_bst bst;
    struct timespec bst_lastupdated;
    
	bool acpi_btp_exists;
	int battery_warning_level;
};


ACPI_SERIAL_DECL(cmbat, "ACPI cmbat");

static int		acpi_cmbat_probe(device_t dev);
static int		acpi_cmbat_attach(device_t dev);
static int		acpi_cmbat_detach(device_t dev);
static int		acpi_cmbat_resume(device_t dev);
static void		acpi_cmbat_notify_handler(ACPI_HANDLE h, UINT32 notify,
			    void *context);
static int		acpi_cmbat_info_expired(struct timespec *lastupdated);
static void		acpi_cmbat_info_updated(struct timespec *lastupdated);
static void		acpi_cmbat_get_bst(void *arg);
static void		acpi_cmbat_get_bif_task(void *arg);
static void		acpi_cmbat_get_bif(void *arg);
static int		acpi_cmbat_bst(device_t dev, struct acpi_bst *bstp);
static int		acpi_cmbat_bif(device_t dev, struct acpi_bif *bifp);
static void		acpi_cmbat_btp_exists(void *arg);
static void		acpi_cmbat_init_battery(void *arg);

static void		acpi_cmbat_sysctls(device_t);
static int 		acpi_cmbat_btp_sysctl(SYSCTL_HANDLER_ARGS);
static int		acpi_cmbat_bif_warning_sysctl(SYSCTL_HANDLER_ARGS);

static device_method_t acpi_cmbat_methods[] = {
    /* Device interface */
    DEVMETHOD(device_probe,	acpi_cmbat_probe),
    DEVMETHOD(device_attach,	acpi_cmbat_attach),
    DEVMETHOD(device_detach,	acpi_cmbat_detach),
    DEVMETHOD(device_resume,	acpi_cmbat_resume),

    /* ACPI battery interface */
    DEVMETHOD(acpi_batt_get_info, acpi_cmbat_bif),
    DEVMETHOD(acpi_batt_get_status, acpi_cmbat_bst),

    DEVMETHOD_END
};

static driver_t acpi_cmbat_driver = {
    "battery",
    acpi_cmbat_methods,
    sizeof(struct acpi_cmbat_softc),
};

static devclass_t acpi_cmbat_devclass;
DRIVER_MODULE(acpi_cmbat, acpi, acpi_cmbat_driver, acpi_cmbat_devclass, 0, 0);
MODULE_DEPEND(acpi_cmbat, acpi, 1, 1, 1);

static int
acpi_cmbat_probe(device_t dev)
{
    static char *cmbat_ids[] = { "PNP0C0A", NULL };

    if (acpi_disabled("cmbat") ||
	ACPI_ID_PROBE(device_get_parent(dev), dev, cmbat_ids) == NULL)
	return (ENXIO);

    device_set_desc(dev, "ACPI Control Method Battery");
    return (0);
}

static int
acpi_cmbat_attach(device_t dev)
{
    int		error;
    ACPI_HANDLE	handle;
    struct acpi_cmbat_softc *sc;

    sc = device_get_softc(dev);
    handle = acpi_get_handle(dev);
    sc->dev = dev;

    timespecclear(&sc->bst_lastupdated);

    error = acpi_battery_register(dev);
    if (error != 0) {
    	device_printf(dev, "registering battery failed\n");
	return (error);
    }

    /*
     * Install a system notify handler in addition to the device notify.
     * Toshiba notebook uses this alternate notify for its battery.
     */
    AcpiInstallNotifyHandler(handle, ACPI_ALL_NOTIFY,
	acpi_cmbat_notify_handler, dev);

    AcpiOsExecute(OSL_NOTIFY_HANDLER, acpi_cmbat_init_battery, dev);

    return (0);
}

static int
acpi_cmbat_detach(device_t dev)
{
    ACPI_HANDLE	handle;

    handle = acpi_get_handle(dev);
    AcpiRemoveNotifyHandler(handle, ACPI_ALL_NOTIFY, acpi_cmbat_notify_handler);
    acpi_battery_remove(dev);

    /*
     * Force any pending notification handler calls to complete by
     * requesting cmbat serialisation while freeing and clearing the
     * softc pointer:
     */
    ACPI_SERIAL_BEGIN(cmbat);
    device_set_softc(dev, NULL);
    ACPI_SERIAL_END(cmbat);

    return (0);
}

static int
acpi_cmbat_resume(device_t dev)
{

    AcpiOsExecute(OSL_NOTIFY_HANDLER, acpi_cmbat_init_battery, dev);
    return (0);
}

static void
acpi_cmbat_notify_handler(ACPI_HANDLE h, UINT32 notify, void *context)
{
    struct acpi_cmbat_softc *sc;
    device_t dev;

    dev = (device_t)context;
    sc = device_get_softc(dev);

    switch (notify) {
    case ACPI_NOTIFY_DEVICE_CHECK:
    case ACPI_BATTERY_BST_CHANGE:
	/*
	 * Clear the last updated time.  The next call to retrieve the
	 * battery status will get the new value for us.
	 */
	timespecclear(&sc->bst_lastupdated);
	break;
    case ACPI_NOTIFY_BUS_CHECK:
    case ACPI_BATTERY_BIF_CHANGE:
	/*
	 * Queue a callback to get the current battery info from thread
	 * context.  It's not safe to block in a notify handler.
	 */
	AcpiOsExecute(OSL_NOTIFY_HANDLER, acpi_cmbat_get_bif_task, dev);
	break;
    }

    acpi_UserNotify("CMBAT", h, notify);
}

static int
acpi_cmbat_info_expired(struct timespec *lastupdated)
{
    struct timespec	curtime;

    ACPI_SERIAL_ASSERT(cmbat);

    if (lastupdated == NULL)
	return (TRUE);
    if (!timespecisset(lastupdated))
	return (TRUE);

    getnanotime(&curtime);
    timespecsub(&curtime, lastupdated, &curtime);
    return (curtime.tv_sec < 0 ||
	    curtime.tv_sec > acpi_battery_get_info_expire());
}

static void
acpi_cmbat_info_updated(struct timespec *lastupdated)
{

    ACPI_SERIAL_ASSERT(cmbat);

    if (lastupdated != NULL)
	getnanotime(lastupdated);
}

static void
acpi_cmbat_get_bst(void *arg)
{
    struct acpi_cmbat_softc *sc;
    ACPI_STATUS	as;
    ACPI_OBJECT	*res;
    ACPI_HANDLE	h;
    ACPI_BUFFER	bst_buffer;
    device_t dev;

    ACPI_SERIAL_ASSERT(cmbat);

    dev = arg;
    sc = device_get_softc(dev);
    h = acpi_get_handle(dev);
    bst_buffer.Pointer = NULL;
    bst_buffer.Length = ACPI_ALLOCATE_BUFFER;

    if (!acpi_cmbat_info_expired(&sc->bst_lastupdated))
	goto end;

    as = AcpiEvaluateObject(h, "_BST", NULL, &bst_buffer);
    if (ACPI_FAILURE(as)) {
	ACPI_VPRINT(dev, acpi_device_get_parent_softc(dev),
		    "error fetching current battery status -- %s\n",
		    AcpiFormatException(as));
	goto end;
    }

    res = (ACPI_OBJECT *)bst_buffer.Pointer;
    if (!ACPI_PKG_VALID(res, 4)) {
	ACPI_VPRINT(dev, acpi_device_get_parent_softc(dev),
		    "battery status corrupted\n");
	goto end;
    }

    if (acpi_PkgInt32(res, 0, &sc->bst.state) != 0)
	goto end;
    if (acpi_PkgInt32(res, 1, &sc->bst.rate) != 0)
	goto end;
    if (acpi_PkgInt32(res, 2, &sc->bst.cap) != 0)
	goto end;
    if (acpi_PkgInt32(res, 3, &sc->bst.volt) != 0)
	goto end;
    acpi_cmbat_info_updated(&sc->bst_lastupdated);

    /* Clear out undefined/extended bits that might be set by hardware. */
    sc->bst.state &= ACPI_BATT_STAT_BST_MASK;
    if ((sc->bst.state & ACPI_BATT_STAT_INVALID) == ACPI_BATT_STAT_INVALID)
	ACPI_VPRINT(dev, acpi_device_get_parent_softc(dev),
	    "battery reports simultaneous charging and discharging\n");

    /* XXX If all batteries are critical, perhaps we should suspend. */
    if (sc->bst.state & ACPI_BATT_STAT_CRITICAL) {
    	if ((sc->flags & ACPI_BATT_STAT_CRITICAL) == 0) {
	    sc->flags |= ACPI_BATT_STAT_CRITICAL;
	    device_printf(dev, "critically low charge!\n");
	}
    } else
	sc->flags &= ~ACPI_BATT_STAT_CRITICAL;

end:
    if (bst_buffer.Pointer != NULL)
	AcpiOsFree(bst_buffer.Pointer);
}

/* XXX There should be a cleaner way to do this locking. */
static void
acpi_cmbat_get_bif_task(void *arg)
{

    ACPI_SERIAL_BEGIN(cmbat);
    acpi_cmbat_get_bif(arg);
    ACPI_SERIAL_END(cmbat);
}

static void
acpi_cmbat_get_bif(void *arg)
{
    struct acpi_cmbat_softc *sc;
    ACPI_STATUS	as;
    ACPI_OBJECT	*res;
    ACPI_HANDLE	h;
    ACPI_BUFFER	bif_buffer;
    device_t dev;

    ACPI_SERIAL_ASSERT(cmbat);

    dev = arg;
    sc = device_get_softc(dev);
    h = acpi_get_handle(dev);
    bif_buffer.Pointer = NULL;
    bif_buffer.Length = ACPI_ALLOCATE_BUFFER;

    as = AcpiEvaluateObject(h, "_BIF", NULL, &bif_buffer);
    if (ACPI_FAILURE(as)) {
	ACPI_VPRINT(dev, acpi_device_get_parent_softc(dev),
		    "error fetching current battery info -- %s\n",
		    AcpiFormatException(as));
	goto end;
    }

    res = (ACPI_OBJECT *)bif_buffer.Pointer;
    if (!ACPI_PKG_VALID(res, 13)) {
	ACPI_VPRINT(dev, acpi_device_get_parent_softc(dev),
		    "battery info corrupted\n");
	goto end;
    }

    if (acpi_PkgInt32(res, 0, &sc->bif.units) != 0)
	goto end;
    if (acpi_PkgInt32(res, 1, &sc->bif.dcap) != 0)
	goto end;
    if (acpi_PkgInt32(res, 2, &sc->bif.lfcap) != 0)
	goto end;
    if (acpi_PkgInt32(res, 3, &sc->bif.btech) != 0)
	goto end;
    if (acpi_PkgInt32(res, 4, &sc->bif.dvol) != 0)
	goto end;
    if (acpi_PkgInt32(res, 5, &sc->bif.wcap) != 0)
	goto end;
    if (acpi_PkgInt32(res, 6, &sc->bif.lcap) != 0)
	goto end;
    if (acpi_PkgInt32(res, 7, &sc->bif.gra1) != 0)
	goto end;
    if (acpi_PkgInt32(res, 8, &sc->bif.gra2) != 0)
	goto end;
    if (acpi_PkgStr(res,  9, sc->bif.model, ACPI_CMBAT_MAXSTRLEN) != 0)
	goto end;
    if (acpi_PkgStr(res, 10, sc->bif.serial, ACPI_CMBAT_MAXSTRLEN) != 0)
	goto end;
    if (acpi_PkgStr(res, 11, sc->bif.type, ACPI_CMBAT_MAXSTRLEN) != 0)
	goto end;
    if (acpi_PkgStr(res, 12, sc->bif.oeminfo, ACPI_CMBAT_MAXSTRLEN) != 0)
	goto end;

end:
    if (bif_buffer.Pointer != NULL)
	AcpiOsFree(bif_buffer.Pointer);
}

static int
acpi_cmbat_bif(device_t dev, struct acpi_bif *bifp)
{
    struct acpi_cmbat_softc *sc;

    sc = device_get_softc(dev);

    /*
     * Just copy the data.  The only value that should change is the
     * last-full capacity, so we only update when we get a notify that says
     * the info has changed.  Many systems apparently take a long time to
     * process a _BIF call so we avoid it if possible.
     */
    ACPI_SERIAL_BEGIN(cmbat);
    bifp->units = sc->bif.units;
    bifp->dcap = sc->bif.dcap;
    bifp->lfcap = sc->bif.lfcap;
    bifp->btech = sc->bif.btech;
    bifp->dvol = sc->bif.dvol;
    bifp->wcap = sc->bif.wcap;
    bifp->lcap = sc->bif.lcap;
    bifp->gra1 = sc->bif.gra1;
    bifp->gra2 = sc->bif.gra2;
    strncpy(bifp->model, sc->bif.model, sizeof(sc->bif.model));
    strncpy(bifp->serial, sc->bif.serial, sizeof(sc->bif.serial));
    strncpy(bifp->type, sc->bif.type, sizeof(sc->bif.type));
    strncpy(bifp->oeminfo, sc->bif.oeminfo, sizeof(sc->bif.oeminfo));
    ACPI_SERIAL_END(cmbat);

    return (0);
}

static int
acpi_cmbat_bst(device_t dev, struct acpi_bst *bstp)
{
    struct acpi_cmbat_softc *sc;

    sc = device_get_softc(dev);

    ACPI_SERIAL_BEGIN(cmbat);
    if (acpi_BatteryIsPresent(dev)) {
	acpi_cmbat_get_bst(dev);
	bstp->state = sc->bst.state;
	bstp->rate = sc->bst.rate;
	bstp->cap = sc->bst.cap;
	bstp->volt = sc->bst.volt;
    } else
	bstp->state = ACPI_BATT_STAT_NOT_PRESENT;
    ACPI_SERIAL_END(cmbat);

    return (0);
}

static void
acpi_cmbat_btp_exists(void *arg) {

    struct acpi_cmbat_softc *sc;
    ACPI_STATUS	as;
    ACPI_HANDLE	h;
    device_t dev;

    ACPI_SERIAL_ASSERT(cmbat);

    dev = arg;
    sc = device_get_softc(dev);
    h = acpi_get_handle(dev);
    
    sc->battery_warning_level=20; /* default warning level */
    as = acpi_SetInteger(h, "_BTP", sc->bif.lfcap / 5);
    
    if(as == AE_NOT_FOUND )
		sc->acpi_btp_exists = FALSE;
		
	else if (ACPI_FAILURE(as)) {
	printf( "error setting _BTP\n");
	sc->acpi_btp_exists = FALSE;
    }
    else 
		sc->acpi_btp_exists = TRUE;
}

static void
acpi_cmbat_init_battery(void *arg)
{
    struct acpi_cmbat_softc *sc;
    int		retry, valid;
    device_t	dev;

    dev = (device_t)arg;
    ACPI_VPRINT(dev, acpi_device_get_parent_softc(dev),
		"battery initialization start\n");

    /*
     * Try repeatedly to get valid data from the battery.  Since the
     * embedded controller isn't always ready just after boot, we may have
     * to wait a while.
     */
    for (retry = 0; retry < ACPI_CMBAT_RETRY_MAX; retry++, AcpiOsSleep(10000)) {
	/*
	 * Batteries on DOCK can be ejected w/ DOCK during retrying.
	 *
	 * If there is a valid softc pointer the device may be in
	 * attaching, attached or detaching state. If the state is
	 * different from attached retry getting the device state
	 * until it becomes stable. This solves a race if the ACPI
	 * notification handler is called during attach, because
	 * device_is_attached() doesn't return non-zero until after
	 * the attach code has been executed.
	 */
	ACPI_SERIAL_BEGIN(cmbat);
	sc = device_get_softc(dev);
	if (sc == NULL) {
	    ACPI_SERIAL_END(cmbat);
	    return;
	}

	if (!acpi_BatteryIsPresent(dev) || !device_is_attached(dev)) {
	    ACPI_SERIAL_END(cmbat);
	    continue;
	}

	/*
	 * Only query the battery if this is the first try or the specific
	 * type of info is still invalid.
	 */
	if (retry == 0 || !acpi_battery_bst_valid(&sc->bst)) {
	    timespecclear(&sc->bst_lastupdated);
	    acpi_cmbat_get_bst(dev);
	}
	if (retry == 0 || !acpi_battery_bif_valid(&sc->bif))
	    acpi_cmbat_get_bif(dev);


	valid = acpi_battery_bst_valid(&sc->bst) &&
	    acpi_battery_bif_valid(&sc->bif);

	if( valid) {
		/* check for optional tables */
		acpi_cmbat_btp_exists(dev);
			
	}
	ACPI_SERIAL_END(cmbat);

	if (valid) {
		/* add cmbat sysctls */
		acpi_cmbat_sysctls(dev);		
	    break;
	}
    }

    if (retry == ACPI_CMBAT_RETRY_MAX) {
	ACPI_VPRINT(dev, acpi_device_get_parent_softc(dev),
		    "battery initialization failed, giving up\n");
    } else {
	ACPI_VPRINT(dev, acpi_device_get_parent_softc(dev),
		    "battery initialization done, tried %d times\n", retry + 1);
    }
}

static void acpi_cmbat_sysctls( device_t dev) {

		struct acpi_cmbat_softc *sc = device_get_softc(dev);
		
		int battery_unit = device_get_unit(dev);
		char *unit = malloc(10, M_ACPICMBAT, M_WAITOK);
		sprintf(unit, "%i", battery_unit);
		
		struct sysctl_oid *cmbat_tree =SYSCTL_ADD_NODE(NULL, SYSCTL_STATIC_CHILDREN(_dev), OID_AUTO, "cmbat", CTLFLAG_RW, 0, "Control Method Batteries");
		struct sysctl_oid *cmbat_oid = SYSCTL_ADD_NODE(NULL, SYSCTL_CHILDREN(cmbat_tree), OID_AUTO, unit, CTLFLAG_RW, 0, "unit number");

		if(sc->acpi_btp_exists) {
			SYSCTL_ADD_PROC(NULL, SYSCTL_CHILDREN(cmbat_oid), OID_AUTO, "Any", CTLTYPE_INT | CTLFLAG_RW, 0, 0, acpi_cmbat_btp_sysctl, "I" ,"battery level warning");
		}
		SYSCTL_ADD_PROC(NULL, SYSCTL_CHILDREN(cmbat_oid), OID_AUTO, "Low", CTLTYPE_INT | CTLFLAG_RW, 0, 0, acpi_cmbat_bif_warning_sysctl, "I" ,"low battery warning");
		SYSCTL_ADD_PROC(NULL, SYSCTL_CHILDREN(cmbat_oid), OID_AUTO, "CriticallyLow", CTLTYPE_INT | CTLFLAG_RW, 0, 0, acpi_cmbat_bif_warning_sysctl, "I" ,"critically low battery warning");

		//free(unit);
}

static int acpi_cmbat_btp_sysctl(SYSCTL_HANDLER_ARGS) {
	
	device_t dev;	
	ACPI_HANDLE h;
	ACPI_STATUS as;
	
	struct acpi_cmbat_softc *sc;
	
	/* find "our" battery */
	struct sysctl_oid *parent = SYSCTL_PARENT(oidp);
	if(!parent)
		return(1);
	long battery_index = strtol(parent->oid_name, NULL, 0);
	
	dev = devclass_get_device( acpi_cmbat_devclass, (int) battery_index);
	if(!dev)
		return(1);
	sc = device_get_softc(dev);
	h = acpi_get_handle(dev);
	
	if(req->newptr) {
		/* write request */
		SYSCTL_IN(req, &sc->battery_warning_level, sizeof(sc->battery_warning_level));
		
		if(sc->battery_warning_level < 1 || sc->battery_warning_level > 99)
			sc->battery_warning_level = 20; /* correct bogus writes */
		

		double newtp = sc->bif.lfcap;
		newtp = newtp * (sc->battery_warning_level);
		newtp = newtp / 100;
		
		printf("oh.. btw: bif.lfcap=%i, bif.lcap=%i, wcap=%i, gra1=%i, gra2=%i, newtp=%i\n", sc->bif.lfcap, sc->bif.lcap, sc->bif.wcap, sc->bif.gra1, sc->bif.gra2, (int) newtp);
		as = acpi_SetInteger(h, "_BTP", (int) newtp);
    
		if (ACPI_FAILURE(as))
			printf( "error setting _BTP\n");
	}
	else /* read request */
		SYSCTL_OUT(req, &sc->battery_warning_level, sizeof(sc->battery_warning_level));

	return 0;
}

static int acpi_cmbat_bif_warning_sysctl(SYSCTL_HANDLER_ARGS) {

	device_t dev;	
	ACPI_HANDLE h;
	
	struct acpi_cmbat_softc *sc;
	
	/* find "our" battery */
	struct sysctl_oid *parent = SYSCTL_PARENT(oidp);
	
	long battery_index = strtol(parent->oid_name, NULL, 0);
	
	dev = devclass_get_device( acpi_cmbat_devclass, (int) battery_index);
	sc = device_get_softc(dev);
	h = acpi_get_handle(dev);
	
	int warning=5; /* default warning level */
		
	if(req->newptr) {
		/* write request */
		SYSCTL_IN(req, &warning, sizeof(warning));
		
		if(warning < 1 || warning > 100)
			warning = 5; 
			
		warning = warning * sc->bif.lfcap /100;
		device_printf(dev, "setting -->%s to %i\n", oidp->oid_name, warning);
		
		if(strncmp(oidp->oid_name, "Low", 3) == 0 )
			sc->bif.wcap = warning;
		else if(strncmp(oidp->oid_name, "CriticallyLow", 13) == 0 )
			sc->bif.lcap = warning;
	}
	else /* read request */ {
		if(strncmp(oidp->oid_name, "Low", 3) == 0 )
		warning = sc->bif.wcap;
		else if(strncmp(oidp->oid_name, "CriticallyLow", 13) == 0 )
			warning = sc->bif.lcap;
	
		if(!warning)
			warning = 5;
		warning = (double) warning *100 / sc->bif.lfcap;
		SYSCTL_OUT(req, &warning, sizeof(warning));
	}
	return 0;
}
