#include "usb_midi.h"

#define IJCRINGBUFFER_IMPLEMENTATION
#include "ijc_ringbuffer.h"

uint32_t get_descriptor_string(USBHost &usb, uint8_t index, uint32_t buffer_size, char *buffer) {
	static uint8_t tmp_buf[64];
	uint32_t rcode = usb.getStrDescr(0, 0, 64, 0, 0, tmp_buf);
	if (rcode)
		return 1;

	uint16_t language = *(uint16_t*)&tmp_buf[2];

	rcode = usb.getStrDescr(0, 0, 64, index, language, tmp_buf);
	if (rcode)
		return 2;

	memset(buffer, 0, buffer_size);

	// dirty unicode to utf8
	for (int i = 2; i < tmp_buf[0]; i += 2) {
		sprintf(buffer, "%s%c", buffer, *(uint16_t*)&tmp_buf[i]);
	}
	return 0;
}



extern void (*gpf_isr)(void);
void (*old_gpf_isr)(void) = nullptr;

ijcringbuffer _ringbuffer;

void UHD_ISR_OVERRIDE(void)
{
	uint32_t pipe = uhd_get_interrupt_pipe_number();
	if (Is_uhd_pipe_interrupt(pipe)) {
		if (Is_uhd_in_received(pipe)) {
			uhd_freeze_pipe(pipe);
			uhd_ack_in_received(pipe);

			uint32_t nb_byte_received = uhd_byte_count(pipe);
			//ISR_TRACE(printf("%d bytes received\r\n", nb_byte_received));

			uint8_t *ptr_ep_data = (uint8_t *)& uhd_get_pipe_fifo_access(pipe, 8);

			if (nb_byte_received > 0) {
				int ret = ijcringbuffer_produce(&_ringbuffer, ptr_ep_data, nb_byte_received);
				if (!ret) {
					ISR_TRACE(printf("ringbuffer full\r\n"));
				}
			}

			uhd_ack_fifocon(pipe);
			uhd_unfreeze_pipe(pipe);
		}
		return;
	}

	// call original isr in uotghs_host.c
	old_gpf_isr();
}


USBMidi::USBMidi(USBHost &usb)
	: _usb(usb)
	, _address(0)
	, _note_on_handler(nullptr)
	, _note_off_handler(nullptr)
	, _control_change_handler(nullptr)
	, _program_change_handler(nullptr)
	, _pitch_bend_handler(nullptr)
	, _raw_midi_handler(nullptr)
{

	ijcringbuffer_init(&_ringbuffer, _ringbuffer_data, RINGBUFFER_SIZE);

	// initialize endpoint data structures
	for (uint8_t i = 0; i < MIDI_MAX_ENDPOINTS; i++) {
		_ep_info[i].deviceEpNum = 0;
		_ep_info[i].hostPipeNum = 0;
		_ep_info[i].maxPktSize = (i) ? 0 : 8;
		_ep_info[i].epAttribs = 0;
		_ep_info[i].bmNakPower = (i) ? USB_NAK_NOWAIT : 4;
	}

	_usb.RegisterDeviceClass(this);
}

uint32_t USBMidi::Init(uint32_t parent, uint32_t port, uint32_t lowspeed)
{
	uint8_t buf[256];
	UsbDevice *device;
	EpInfo *address0_epinfo;

	AddressPool &address_pool = _usb.GetAddressPool();

	device = address_pool.GetUsbDevicePtr(0);
	if (!device)
		return USB_ERROR_ADDRESS_NOT_FOUND_IN_POOL;

	if (!device->epinfo)
		return USB_ERROR_EPINFO_IS_NULL;

	address0_epinfo = device->epinfo;
	device->epinfo = _ep_info;

	uint32_t rcode = _usb.getDevDescr(0, 0, sizeof(USB_DEVICE_DESCRIPTOR), buf);
	USB_DEVICE_DESCRIPTOR *device_desc = (USB_DEVICE_DESCRIPTOR*)buf;

#if defined(PRINT_MIDI_INFO)
	// parse descriptor strings
	if (device_desc->iManufacturer != 0) {
		char vendor[64];
		uint32_t r = get_descriptor_string(_usb, device_desc->iManufacturer, 64, vendor);
		printf("vendor: %s\r\n", vendor);
	}
	if (device_desc->iProduct != 0) {
		char product[64];
		get_descriptor_string(_usb, device_desc->iProduct, 64, product);
		printf("product: %s\r\n", product);
	}
#endif

	device->epinfo = address0_epinfo;
	_address = address_pool.AllocAddress(parent, 0, port);

	_ep_info[0].maxPktSize = device_desc->bMaxPacketSize0;

	rcode = _usb.setAddr(0, 0, _address);
	device->lowspeed = 0;

	device = address_pool.GetUsbDevicePtr(_address);

	rcode = _usb.setEpInfoEntry(_address, 1, _ep_info);
	if (rcode) {
		MIDI_TRACE(printf("failed to set ep info entry\r\n");)
	}

	uint8_t num_conf = device_desc->bNumConfigurations;
	uint8_t num_endpoints = 0;
	uint8_t conf_value = 0;
	for (uint8_t i = 0; i < num_conf; ++i) {
		parse_config_descriptors(_address, i, num_endpoints, conf_value);
	}

	_usb.setEpInfoEntry(_address, num_endpoints, _ep_info);
	_usb.setConf(_address, 0, conf_value);

	attach_isr();
	start_in_generation(1);

	return 0;
}

void USBMidi::start_in_generation(uint32_t endpoint)
{

	// Start in token generation
	uint32_t pipe = _ep_info[endpoint].hostPipeNum;
	MIDI_TRACE(printf("start endless in token generation for pipe: %d\r\n", pipe));

	if (!Is_uhd_pipe_enabled(pipe)) {
		MIDI_TRACE(printf("pipe not enabled\r\n", pipe));

	}

	// Clear interrupt flags
	uhd_ack_setup_ready(pipe);
	uhd_ack_in_received(pipe);
	uhd_ack_out_ready(pipe);
	uhd_ack_short_packet(pipe);
	uhd_ack_nak_received(pipe);

	uhd_enable_continuous_in_mode(pipe);

	uhd_enable_pipe_interrupt(pipe);
	uhd_enable_in_received_interrupt(pipe);

	uhd_unfreeze_pipe(pipe);
}

void USBMidi::attach_isr()
{
	if (gpf_isr != UHD_ISR_OVERRIDE) {
		old_gpf_isr = gpf_isr;
		gpf_isr = UHD_ISR_OVERRIDE;
	}
}

uint32_t USBMidi::Release()
{

	return 0;
}

#define BUFFER_SIZE 256
void USBMidi::parse_config_descriptors(uint32_t address, uint8_t config_index, uint8_t &num_endpoints, uint8_t &conf_value)
{
	uint8_t buf[BUFFER_SIZE];
	uint32_t rcode;
	uint32_t total_size;

	// get the first 4 bytes of the descriptor
	rcode = _usb.getConfDescr(address, 0, 4, config_index, buf);
	if(rcode)
		return;

	USB_CONFIGURATION_DESCRIPTOR *config_desc = (USB_CONFIGURATION_DESCRIPTOR*)buf;
	total_size = config_desc->wTotalLength;

	if(total_size > BUFFER_SIZE)
		total_size = BUFFER_SIZE;

	rcode = _usb.getConfDescr(address, 0, total_size, config_index, buf);
	if(rcode)
		return;

	uint8_t *ptr = buf;
	while (ptr < buf + total_size) {
		uint8_t desc_length = *ptr;
		uint8_t desc_type = *(ptr+1);

		switch (desc_type)
		{
		case USB_DESCRIPTOR_CONFIGURATION:
		{
			USB_CONFIGURATION_DESCRIPTOR *desc = (USB_CONFIGURATION_DESCRIPTOR*)ptr;
			conf_value = desc->bConfigurationValue;

			DESC_TRACE(printf("\r\nUSB_CONFIGURATION_DESCRIPTOR\r\n"));
			DESC_TRACE(printf("bLength: %d\r\n", desc->bLength));
			DESC_TRACE(printf("bDescriptorType: 0x%x\r\n", desc->bDescriptorType));
			DESC_TRACE(printf("wTotalLength: %d\r\n", desc->wTotalLength));
			DESC_TRACE(printf("bNumInterfaces: %d\r\n", desc->bNumInterfaces));
			DESC_TRACE(printf("bConfigurationValue: %d\r\n", desc->bConfigurationValue));
			DESC_TRACE(printf("iConfiguration: %d\r\n", desc->iConfiguration));
			DESC_TRACE(printf("bmAttributes: 0x%x\r\n", desc->bmAttributes));
			//DESC_TRACE(printf("MaxPower: %d\r\n", desc->MaxPower));

			break;
		}
		case USB_DESCRIPTOR_INTERFACE:
		{
			USB_INTERFACE_DESCRIPTOR *desc = (USB_INTERFACE_DESCRIPTOR*)ptr;
			DESC_TRACE(printf("\r\nUSB_INTERFACE_DESCRIPTOR\r\n"));
			DESC_TRACE(printf("bLength: %d\r\n", desc->bLength));
			DESC_TRACE(printf("bDescriptorType: 0x%x\r\n", desc->bDescriptorType));
			DESC_TRACE(printf("bInterfaceNumber: %d\r\n", desc->bInterfaceNumber));
			DESC_TRACE(printf("bAlternateSetting: 0x%x\r\n", desc->bAlternateSetting));
			DESC_TRACE(printf("bNumEndpoints: %d\r\n", desc->bNumEndpoints));
			DESC_TRACE(printf("bInterfaceClass: 0x%x\r\n", desc->bInterfaceClass));
			DESC_TRACE(printf("bInterfaceSubClass: 0x%x\r\n", desc->bInterfaceSubClass));
			DESC_TRACE(printf("bInterfaceProtocol: 0x%x\r\n", desc->bInterfaceProtocol));
			DESC_TRACE(printf("iInterface: %d\r\n", desc->iInterface));

			if (desc->bInterfaceClass == USB_CLASS_AUDIO &&
				desc->bInterfaceSubClass == USB_SUBCLASS_MIDISTREAMING) {
				MIDI_TRACE(printf("Midi device found\r\n");)
			}
			break;
		}
		case USB_DESCRIPTOR_ENDPOINT:
		{
			USB_ENDPOINT_DESCRIPTOR *desc = (USB_ENDPOINT_DESCRIPTOR*)ptr;
			DESC_TRACE(printf("\r\nUSB_ENDPOINT_DESCRIPTOR\r\n"));
			DESC_TRACE(printf("bLength: %d\r\n", desc->bLength));
			DESC_TRACE(printf("bDescriptorType: 0x%x\r\n", desc->bDescriptorType));
			DESC_TRACE(printf("bEndpointAddress: 0x%x (%s)\r\n", desc->bEndpointAddress, \ 
				((desc->bEndpointAddress & USB_ENDPOINT_DIRECTION_MASK) ? "IN" : "OUT")));

			DESC_TRACE(printf("bmAttributes: 0x%x (%s)\r\n", desc->bmAttributes, \
				(((desc->bmAttributes & USB_ENDPOINT_TYPE_MASK) == USB_ENDPOINT_TYPE_CONTROL) ? "Control" : \
				((desc->bmAttributes & USB_ENDPOINT_TYPE_MASK) == USB_ENDPOINT_TYPE_ISOCHRONOUS) ? "Isochronous" : \
				((desc->bmAttributes & USB_ENDPOINT_TYPE_MASK) == USB_ENDPOINT_TYPE_BULK) ? "Bulk" : \
				"Interrupt")));

			DESC_TRACE(printf("wMaxPacketSize: %d\r\n", desc->wMaxPacketSize));
			DESC_TRACE(printf("bInterval: 0x%x\r\n", desc->bInterval));



			if ((desc->bmAttributes & bmUSB_TRANSFER_TYPE) == USB_ENDPOINT_TYPE_BULK) {
				// select in or out end point index
				uint8_t ep_index = ((desc->bEndpointAddress & 0x80) == 0x80) ? 1 : 2;


				_ep_info[ep_index].deviceEpNum = desc->bEndpointAddress & 0x0F;
				_ep_info[ep_index].maxPktSize = desc->wMaxPacketSize;

				uint32_t pipe = UHD_Pipe_Alloc(_address, _ep_info[ep_index].deviceEpNum,
					UOTGHS_HSTPIPCFG_PTYPE_BLK, (ep_index == 1) ? UOTGHS_HSTPIPCFG_PTOKEN_IN : UOTGHS_HSTPIPCFG_PTOKEN_OUT,
					_ep_info[ep_index].maxPktSize, 0, UOTGHS_HSTPIPCFG_PBK_1_BANK);

				_ep_info[ep_index].hostPipeNum = pipe;
				MIDI_TRACE(printf("allocated pipe %d for endpoint: %d\r\n", pipe, ep_index));
				num_endpoints++;
			}
			break;
		}
		default:
			break;
		}
		ptr += desc_length;
	}

	MIDI_TRACE(printf("config parse complete: %d\r\n", config_index));
}

uint32_t USBMidi::Poll()
{
	uint32_t available;
	while ((available = ijcringbuffer_consumeable_size_continuous(&_ringbuffer)) > 0) {
		uint8_t *buffer = (uint8_t*)ijcringbuffer_peek(&_ringbuffer);

		if (_raw_midi_handler) {
			_raw_midi_handler(available, buffer);
		}

		uint8_t header = buffer[1] & 0xf0;

		switch (header) {
		case 0x80:	 // note off
			if(_note_off_handler) 
				_note_off_handler(buffer[1] & 0x0f, buffer[2], buffer[3]);
		break;
		case 0x90:	// note on
			if (buffer[3] == 0) { // if velocity is 0 this is a note off
				if(_note_off_handler) 
					_note_off_handler(buffer[1] & 0x0f, buffer[2], buffer[3]);
			} else {
				if(_note_on_handler) 
					_note_on_handler(buffer[1] & 0x0f, buffer[2], buffer[3]);
			}
			break;
		case 0xb0: // Control change
			if(_control_change_handler) 
				_control_change_handler(buffer[1] & 0x0f, buffer[2], buffer[3]);
			break;
		case 0xe0: // pitch bend change
			if(_pitch_bend_handler)
				_pitch_bend_handler(buffer[1] & 0x0f, (int)((buffer[2] & 0x7f) | ((buffer[3] & 0x7f) << 7)) + MIDI_PITCHBEND_MIN);
			break;
		case 0xc0: // program change
			if(_program_change_handler)
				_program_change_handler(buffer[1] & 0x0f, buffer[2]);
			break;
		default:
			// Not implemented midi message
			break;
		}

		ijcringbuffer_consume(&_ringbuffer, available);

	}
}

