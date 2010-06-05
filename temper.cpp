// Thanks to Relavak Labs
//  <http://relavak.wordpress.com/2009/10/17/temper-temperature-sensor-linux-driver/>

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

    static void check (int e) {
	if (e < 0)
	    throw usb_error (libusb_error (e));
    }
};

struct claim_interface {
    bool try_attach;
    shared_ptr<libusb_device_handle> h;
    int interface;

    claim_interface (shared_ptr<libusb_device_handle> h, int interface):
	try_attach (false),
	h (h),
	interface (interface)
    {
	{
	    int r = libusb_kernel_driver_active (h.get (), interface);
	    usb_error::check (r);
	    try_attach = r;
	}

	if (try_attach)
	    usb_error::check (libusb_detach_kernel_driver (h.get (), interface));

	usb_error::check (libusb_claim_interface (h.get (), interface));
    }

    ~claim_interface () try {
	usb_error::check (libusb_release_interface (h.get (), interface));

	if (try_attach)
	    usb_error::check (libusb_attach_kernel_driver (h.get (), interface));
    } catch (const usb_error& e) {
	std::cerr << __FILE__ << ":" << __LINE__ << " (" << __func__ << "): " << e.what ();
    }
};

typedef std::tr1::array<unsigned int, 8> msg;

void send_data (shared_ptr<libusb_device_handle> dh, msg data) {
    std::tr1::array<unsigned char, 32> buf;
    std::copy (data.begin (), data.end (), buf.begin ());
    std::fill (buf.begin () + data.size (), buf.end (), 0);

    int r = libusb_control_transfer (dh.get (),
	0x21, 0x09,
	0x0200, 0x0001,
	&buf[0], buf.size (),
	1000);
    usb_error::check (r);
    if (r != 32) {
	std::ostringstream ss;
	ss << "wrong number of bytes written: " << r;
	throw std::runtime_error (ss.str ());
    }
}

std::tr1::array<unsigned char, 256> recv_data (shared_ptr<libusb_device_handle> dh) {
    std::tr1::array<unsigned char, 256> result;
    int r = libusb_control_transfer (dh.get (),
	0xa1, 0x01,
	0x0300, 0x0001,
	&result[0], result.size (),
	1000);
    usb_error::check (r);
    if (r < 2) {
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

const msg select_device = {{10, 11, 12, 13, 0, 0, 2, 0}};
const msg increase_precision = {{0x43, 0, 0, 0, 0, 0, 0, 0}};
const msg padding = {{0, 0, 0, 0, 0, 0, 0, 0}};
const msg get_temperature = {{0x54, 0, 0, 0, 0, 0, 0, 0}};
const msg close_device = {{10, 11, 12, 13, 0, 0, 1, 0}};

int main () try {
    shared_ptr<libusb_context> usb = usb_open ();

    shared_ptr<libusb_device_handle> dh = usb_device_get (usb, 0x1130, 0x660c);

    usb_error::check (libusb_set_configuration (dh.get (), 1));
    claim_interface i1 (dh, 0);
    claim_interface i2 (dh, 1);

    send_data (dh, select_device);

    send_data (dh, increase_precision);
    for (int i = 0; i < 7; i++) send_data (dh, padding);

    send_data (dh, get_temperature);
    for (int i = 0; i < 7; i++) send_data (dh, padding);

    send_data (dh, close_device);

    std::tr1::array<unsigned char, 256> d = recv_data (dh);

    /*std::ostringstream h;
    h << std::hex << "0x" << int (d[0]) << " 0x" << int (d[1]);
    std::cout << h.str () << std::endl;*/

    //std::cout << ((d[0] << 8) + (d[1] & 0xff)) << std::endl;

    //std::cout << int (d[0]) << '.' << int (d[1]/16) << std::endl;
    std::cout << double (d[0]) + double (d[1])/256 << std::endl;

    return EXIT_SUCCESS;
} catch (std::exception& e) {
    std::cerr << "exception: " << e.what () << std::endl;
    return EXIT_FAILURE;
}
