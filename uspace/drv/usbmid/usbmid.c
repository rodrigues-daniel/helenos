/*
 * Copyright (c) 2011 Vojtech Horky
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * - Redistributions of source code must retain the above copyright
 *   notice, this list of conditions and the following disclaimer.
 * - Redistributions in binary form must reproduce the above copyright
 *   notice, this list of conditions and the following disclaimer in the
 *   documentation and/or other materials provided with the distribution.
 * - The name of the author may not be used to endorse or promote products
 *   derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/** @addtogroup drvusbmid
 * @{
 */
/**
 * @file
 * Helper functions.
 */
#include <errno.h>
#include <str_error.h>
#include <stdlib.h>
#include <usb_iface.h>
#include <usb/ddfiface.h>
#include <usb/pipes.h>
#include <usb/classes/classes.h>
#include <usb/recognise.h>
#include "usbmid.h"

/** Callback for DDF USB interface. */
static int usb_iface_get_address_impl(ddf_fun_t *fun, devman_handle_t handle,
    usb_address_t *address)
{
	return usb_iface_get_address_hub_impl(fun, handle, address);
}

/** Callback for DDF USB interface. */
static int usb_iface_get_interface_impl(ddf_fun_t *fun, devman_handle_t handle,
    int *iface_no)
{
	assert(fun);

	usbmid_interface_t *iface = fun->driver_data;
	assert(iface);

	if (iface_no != NULL) {
		*iface_no = iface->interface_no;
	}

	return EOK;
}

/** DDF interface of the child - interface function. */
static usb_iface_t child_usb_iface = {
	.get_hc_handle = usb_iface_get_hc_handle_hub_child_impl,
	.get_address = usb_iface_get_address_impl,
	.get_interface = usb_iface_get_interface_impl
};

/** Operations for children - interface functions. */
static ddf_dev_ops_t child_device_ops = {
	.interfaces[USB_DEV_IFACE] = &child_usb_iface
};

/** Operations of the device itself. */
static ddf_dev_ops_t mid_device_ops = {
	.interfaces[USB_DEV_IFACE] = &usb_iface_hub_impl
};

/** Create new USB multi interface device.
 *
 * @param dev Backing generic DDF device.
 * @return New USB MID device.
 * @retval NULL Error occured.
 */
usbmid_device_t *usbmid_device_create(ddf_dev_t *dev)
{
	usbmid_device_t *mid = malloc(sizeof(usbmid_device_t));
	if (mid == NULL) {
		usb_log_error("Out of memory (wanted %zu bytes).\n",
		    sizeof(usbmid_device_t));
		return NULL;
	}

	int rc;
	rc = usb_device_connection_initialize_from_device(&mid->wire, dev);
	if (rc != EOK) {
		usb_log_error("Failed to initialize `USB wire': %s.\n",
		    str_error(rc));
		free(mid);
		return NULL;
	}

	rc = usb_endpoint_pipe_initialize_default_control(&mid->ctrl_pipe,
	    &mid->wire);
	if (rc != EOK) {
		usb_log_error("Failed to initialize control pipe: %s.\n",
		    str_error(rc));
		free(mid);
		return NULL;
	}

	mid->dev = dev;
	(void) &mid_device_ops;

	return mid;
}

/** Create new interface for USB MID device.
 *
 * @param fun Backing generic DDF device function (representing interface).
 * @param iface_no Interface number.
 * @return New interface.
 * @retval NULL Error occured.
 */
usbmid_interface_t *usbmid_interface_create(ddf_fun_t *fun, int iface_no)
{
	usbmid_interface_t *iface = malloc(sizeof(usbmid_interface_t));
	if (iface == NULL) {
		usb_log_error("Out of memory (wanted %zuB).\n",
		    sizeof(usbmid_interface_t));
		return NULL;
	}

	iface->fun = fun;
	iface->interface_no = iface_no;

	return iface;
}


/** Spawn new child device from one interface.
 *
 * @param parent Parent MID device.
 * @param device_descriptor Device descriptor.
 * @param interface_descriptor Interface descriptor.
 * @return Error code.
 */
int usbmid_spawn_interface_child(usbmid_device_t *parent,
    const usb_standard_device_descriptor_t *device_descriptor,
    const usb_standard_interface_descriptor_t *interface_descriptor)
{
	ddf_fun_t *child = NULL;
	char *child_name = NULL;
	usbmid_interface_t *child_as_interface = NULL;
	int rc;

	/*
	 * Name is class name followed by interface number.
	 * The interface number shall provide uniqueness while the
	 * class name something humanly understandable.
	 */
	rc = asprintf(&child_name, "%s%d",
	    usb_str_class(interface_descriptor->interface_class),
	    (int) interface_descriptor->interface_number);
	if (rc < 0) {
		goto error_leave;
	}

	/* Create the device. */
	child = ddf_fun_create(parent->dev, fun_inner, child_name);
	if (child == NULL) {
		rc = ENOMEM;
		goto error_leave;
	}



	child_as_interface = usbmid_interface_create(child,
	    (int) interface_descriptor->interface_number);
	if (child_as_interface == NULL) {
		rc = ENOMEM;
		goto error_leave;
	}

	child->driver_data = child_as_interface;
	child->ops = &child_device_ops;

	rc = usb_device_create_match_ids_from_interface(device_descriptor,
	    interface_descriptor,
	    &child->match_ids);
	if (rc != EOK) {
		goto error_leave;
	}

	rc = ddf_fun_bind(child);
	if (rc != EOK) {
		goto error_leave;
	}

	return EOK;

error_leave:
	if (child != NULL) {
		child->name = NULL;
		/* This takes care of match_id deallocation as well. */
		ddf_fun_destroy(child);
	}
	if (child_name != NULL) {
		free(child_name);
	}
	if (child_as_interface != NULL) {
		free(child_as_interface);
	}

	return rc;
}

/**
 * @}
 */
