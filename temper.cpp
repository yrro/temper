// Thanks to Relavak Labs
//  <http://relavak.wordpress.com/2009/10/17/temper-temperature-sensor-linux-driver/>
// and OpenBSD's uthumb driver
//  <http://www.openbsd.org/cgi-bin/cvsweb/src/sys/dev/usb/uthum.c>

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <sstream>

#include <tr1/array>
#include <tr1/functional>
#include <tr1/memory>

#include <libusb.h>

using std::tr1::shared_ptr;

struct usb_error: std::exception {
    libusb_error e;

    usb_error (libusb_error e): e (e) {}

    const char* what () const throw () {
	switch (e) {
	case LIBUSB_SUCCESS: return "Success (no error)";
	case LIBUSB_ERROR_IO: return "Input/output error";
	case LIBUSB_ERROR_INVALID_PARAM: return "Invalid parameter";
	case LIBUSB_ERROR_ACCESS: return "Access denied";
	case LIBUSB_ERROR_NO_DEVICE: return "No such device";
	case LIBUSB_ERROR_NOT_FOUND: return "Entity not found";
	case LIBUSB_ERROR_BUSY: return "Resource busy";
	case LIBUSB_ERROR_TIMEOUT: return "Operation timed out";
	case LIBUSB_ERROR_PIPE: return "Pipe error";
	case LIBUSB_ERROR_INTERRUPTED: return "System call interrupted";
	case LIBUSB_ERROR_NO_MEM: return "Insufficient memory";
	case LIBUSB_ERROR_NOT_SUPPORTED: return "Operation not supported";
	case LIBUSB_ERROR_OTHER: return "Other error";
	case LIBUSB_ERROR_OVERFLOW: return "Overflow";
	}

	std::ostringstream ss;
	ss << e;
	return ss.str ().c_str ();
    }

    static int check (int e) {
	if (e < 0)
	    throw usb_error (libusb_error (e));
	return e;
    }
};

struct usb_attach_interface {
    shared_ptr<libusb_device_handle> h;
    int interface;
    bool was_attached;

    usb_attach_interface (shared_ptr<libusb_device_handle> h, int interface):
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
	std::cerr << __FILE__ << ":" << __LINE__ << " (" << __func__ << "): " << e.what ();
    }
};

struct usb_claim_interface {
    shared_ptr<libusb_device_handle> h;
    int interface;

    usb_claim_interface (shared_ptr<libusb_device_handle> h, int interface):
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

typedef std::tr1::array<unsigned char, 32> msg32;
typedef std::tr1::array<unsigned char, 256> msg256;

enum hid_req {
    get_report = 0x01,
    set_report = 0x09
};

void usb_send (shared_ptr<libusb_device_handle> dh, msg32 data) {
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

msg256 usb_recv (shared_ptr<libusb_device_handle> dh) {
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

shared_ptr<libusb_context> usb_open () {
    libusb_context* p;
    usb_error::check (libusb_init (&p));
    return shared_ptr<libusb_context> (p, libusb_exit);
}

std::pair<shared_ptr<libusb_device*>, ssize_t> usb_device_list (shared_ptr<libusb_context> usb) {
    libusb_device** p;
    ssize_t n = libusb_get_device_list (usb.get (), &p);
    return std::make_pair (shared_ptr<libusb_device*> (p, std::tr1::bind (libusb_free_device_list, std::tr1::placeholders::_1, 1)), n);
}

shared_ptr<libusb_device_handle> usb_device_get (shared_ptr<libusb_context> usb, uint16_t vendor, uint16_t product) {
    std::pair<shared_ptr<libusb_device*>, ssize_t> list = usb_device_list (usb);
    for (libusb_device** dev = list.first.get (); dev != list.first.get () + list.second; ++dev) {
	libusb_device_descriptor d;
	usb_error::check (libusb_get_device_descriptor (*dev, &d));
	if (d.idVendor == vendor && d.idProduct == product) {
	    libusb_device_handle* p;
	    usb_error::check (libusb_open (*dev, &p));
	    return shared_ptr<libusb_device_handle> (p, libusb_close);
	}
    }

    throw std::runtime_error ("could not find device");
}

void send_cmd (shared_ptr<libusb_device_handle> dh, unsigned char cmd) {

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

msg256 read_data (shared_ptr<libusb_device_handle> dh, unsigned char cmd) {
    send_cmd (dh, cmd);

    // hey, give me the data!
    msg32 b = {{10, 11, 12, 13, 0, 0, 1, 0}};
    usb_send (dh, b);

    return usb_recv (dh);
}

enum cmds {
    cmd_getdata_ntc   = 0x41,
    cmd_reset0        = 0x43,
    cmd_reset1        = 0x44,
    cmd_getdata       = 0x48,
    cmd_devtype       = 0x52,
    cmd_getdata_outer = 0x53,
    cmd_getdata_inner = 0x54
};

enum dev_types {
    dev_type_temperhum  = 0x5a53,
    dev_type_temperhum2 = 0x5a57,
    dev_type_temper1    = 0x5857,
    dev_type_temper2    = 0x5957,
    dev_type_temperntc  = 0x5b57
};

int main () try {
    shared_ptr<libusb_context> usb = usb_open ();

    shared_ptr<libusb_device_handle> dh = usb_device_get (usb, 0x1130, 0x660c);

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
	std::copy (dinfo_raw.begin (), dinfo_raw.begin () + sizeof (dev_info), reinterpret_cast<unsigned char*> (&dinfo));

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
	std::cout << float (d[0]) + float (d[1])/256 << std::endl;
    }

    return EXIT_SUCCESS;
} catch (std::exception& e) {
    std::cerr << "exception: " << e.what () << std::endl;
    return EXIT_FAILURE;
}
