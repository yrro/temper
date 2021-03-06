// Thanks to Relavak Labs
//  <http://relavak.wordpress.com/2009/10/17/temper-temperature-sensor-linux-driver/>
// and OpenBSD's uthumb driver
//  <http://www.openbsd.org/cgi-bin/cvsweb/src/sys/dev/usb/uthum.c>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <sstream>

#include <libusb.h>

struct usb_error: std::exception {
    libusb_error e;

    usb_error (libusb_error e): e (e) {}

    const char* what () const noexcept {
        return libusb_error_name(e);
    }

    static int check (int e) {
	if (e < 0)
	    throw usb_error (libusb_error (e));
	return e;
    }
};

std::unique_ptr<libusb_context, decltype(&libusb_exit)> usb_open() {
    libusb_context* p;
    usb_error::check(libusb_init(&p));
    return std::unique_ptr<libusb_context, decltype(&libusb_exit)>(p, libusb_exit);
}

struct usb_attach_interface {
    std::shared_ptr<libusb_device_handle> h;
    int interface;
    bool was_attached;

    usb_attach_interface (std::shared_ptr<libusb_device_handle> h, int interface):
	h (h),
	interface (interface),
	was_attached (usb_error::check (libusb_kernel_driver_active (h.get (), interface)))
    {
	if (was_attached)
	    usb_error::check (libusb_detach_kernel_driver (h.get (), interface));
    }

    ~usb_attach_interface () try {
	if (was_attached)
	    usb_error::check (libusb_attach_kernel_driver (h.get (), interface));
    } catch (const usb_error& e) {
	std::cerr << __FILE__ << ":" << __LINE__ << " (" << __func__ << "): " << e.what () << '\n';
    }
};

struct usb_claim_interface {
    std::shared_ptr<libusb_device_handle> h;
    int interface;

    usb_claim_interface (std::shared_ptr<libusb_device_handle> h, int interface):
	h (h),
	interface (interface)
    {
	usb_error::check (libusb_claim_interface (h.get (), interface));
    }

    ~usb_claim_interface () try {
	usb_error::check (libusb_release_interface (h.get (), interface));
    } catch (const usb_error& e) {
	std::cerr << __FILE__ << ":" << __LINE__ << " (" << __func__ << "): " << e.what ();
    }
};

typedef std::array<unsigned char, 32> msg32;
typedef std::array<unsigned char, 256> msg256;

enum hid_req {
    get_report = 0x01,
    set_report = 0x09
};

void usb_send (std::shared_ptr<libusb_device_handle> dh, msg32 data) {
    int r = libusb_control_transfer (dh.get (),
	LIBUSB_ENDPOINT_OUT | LIBUSB_REQUEST_TYPE_CLASS | LIBUSB_RECIPIENT_INTERFACE, set_report,
	0x0200, 0x0001,
	&data[0], data.size (),
	1000);
    usb_error::check (r);
    if (r != int (data.size ())) {
	std::ostringstream ss;
	ss << "wrong number of bytes written: " << r;
	throw std::runtime_error (ss.str ());
    }
}

msg256 usb_recv (std::shared_ptr<libusb_device_handle> dh) {
    msg256 result;
    int r = libusb_control_transfer (dh.get (),
	LIBUSB_ENDPOINT_IN | LIBUSB_REQUEST_TYPE_CLASS | LIBUSB_RECIPIENT_INTERFACE, get_report,
	0x0300, 0x0001,
	&result[0], result.size (),
	1000);
    usb_error::check (r);
    if (r < int (result.size ())) {
	std::ostringstream ss;
	ss << "wrong number of bytes read: " << r;
	throw std::runtime_error (ss.str ());
    }
    return result;
}

auto libusb_device_list_deleter = [](libusb_device** d) { libusb_free_device_list(d, 1); };

std::pair<std::unique_ptr<libusb_device*, decltype(libusb_device_list_deleter)>, ssize_t> usb_device_list (libusb_context* usb) {
    libusb_device** p;
    ssize_t n = libusb_get_device_list (usb, &p);
    return std::make_pair (
        std::unique_ptr<libusb_device*, decltype(libusb_device_list_deleter)> (p, libusb_device_list_deleter),
        n
    );
}

std::shared_ptr<libusb_device_handle> usb_device_get (libusb_context* usb, uint16_t vendor, uint16_t product) {
    auto list = usb_device_list (usb);
    for (libusb_device** dev = list.first.get (); dev != list.first.get () + list.second; ++dev) {
	libusb_device_descriptor d;
	usb_error::check (libusb_get_device_descriptor (*dev, &d));
	if (d.idVendor == vendor && d.idProduct == product) {
	    libusb_device_handle* p;
	    usb_error::check (libusb_open (*dev, &p));
	    return std::shared_ptr<libusb_device_handle> (p, libusb_close);
	}
    }

    throw std::runtime_error ("could not find device");
}

void send_cmd (std::shared_ptr<libusb_device_handle> dh, unsigned char cmd) {

    // hey, here comes a command!
    {
	msg32 b = {{0x0a, 0x0b, 0x0c, 0x0d, 0x00, 0x00, 0x02, 0x00}};
	usb_send (dh, b);
    }

    // issue the command
    {
	msg32 b = {{0}};
	b[0] = cmd;
	usb_send (dh, b);
    }

    // i2c bus padding
    {
	msg32 b = {{0}};
	for (int i = 0; i < 7; ++i)
	    usb_send (dh, b);
    }
}

msg256 read_data (std::shared_ptr<libusb_device_handle> dh, unsigned char cmd) {
    send_cmd (dh, cmd);

    // hey, give me the data!
    msg32 b = {{10, 11, 12, 13, 0, 0, 1, 0}};
    usb_send (dh, b);

    return usb_recv (dh);
}

enum cmds {
    cmd_getdata_ntc   = 0x41,
    cmd_reset0	      = 0x43,
    cmd_reset1	      = 0x44,
    cmd_getdata       = 0x48,
    cmd_devtype       = 0x52,
    cmd_getdata_outer = 0x53,
    cmd_getdata_inner = 0x54
};

enum dev_types {
    dev_type_temperhum	= 0x5a53,
    dev_type_temperhum2 = 0x5a57,
    dev_type_temper1	= 0x5857,
    dev_type_temper2	= 0x5957,
    dev_type_temperntc	= 0x5b57
};

int main () try {
    auto usb = usb_open();

    std::shared_ptr<libusb_device_handle> dh = usb_device_get (usb.get(), 0x1130, 0x660c);

    usb_attach_interface a1 (dh, 0);
    usb_attach_interface a2 (dh, 1);

    usb_error::check (libusb_set_configuration (dh.get (), 1));

    usb_claim_interface i1 (dh, 0);
    usb_claim_interface i2 (dh, 1);

    // init
    {
	struct dev_info {
	    uint16_t dev_type;
	    uint8_t cal[2][2];
	    // OpenBSD repeatedly issues the devtype command until this != 0x53
	    // Maybe this is necessary if the device has just been plugged in
	    // and has not settled yet?
	    uint8_t footer;
	} dinfo;
	msg256 dinfo_raw = read_data (dh, cmd_devtype);
	std::copy (std::begin(dinfo_raw), std::end(dinfo_raw), reinterpret_cast<unsigned char*> (&dinfo));

	//int val;
	switch (dinfo.dev_type) {
	case dev_type_temper1:
	    send_cmd (dh, cmd_reset0);
	    /*val = (dinfo.cal[0][0] - 0x14) * 100;
	    val += dinfo.cal[0][1] * 10;
	    std::cerr << "calibration: " << val << std::endl;*/
	    break;
	default:
	    throw std::runtime_error ("unknwon device type");
	}
    }

    // read
    {
	msg256 d = read_data (dh, cmd_getdata_inner);

	// raw values
	/*
	std::ostringstream h;
	h << std::hex << "0x" << int (d[0]) << " 0x" << int (d[1]);
	std::cout << h.str () << std::endl;
	std::cout << ((d[0] << 8) + (d[1] & 0xff)) << std::endl;
	*/

	// from OpenBSD
	//std::cout << d[0] * 100 + (d[1] >> 4) * 25 / 4 << std::endl;

	// easy way
	std::cout << float (d[0]) + float (d[1])/256 << '\n';
    }

    return EXIT_SUCCESS;
} catch (std::exception& e) {
    std::cerr << "exception: " << e.what () << std::endl;
    return EXIT_FAILURE;
}

// vim: ts=8 sts=4 sw=4 et
