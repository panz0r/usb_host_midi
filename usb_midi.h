#pragma once

#include <Arduino.h>
#include <Usb.h>


//#define MIDI_TRACE(x) x
#define MIDI_TRACE(x)

#define DESC_TRACE(x) (x)
//#define DESC_TRACE(x)

//#define ISR_TRACE(x) (x)
#define ISR_TRACE(x)

#define PRINT_MIDI_INFO (1)


#define MIDI_MAX_ENDPOINTS 5 
#define USB_SUBCLASS_MIDISTREAMING 3

#define RINGBUFFER_SIZE 128	// needs to be power of two

#define MIDI_PITCHBEND_MIN      -8192
#define MIDI_PITCHBEND_MAX      8191

class USBMidi : public USBDeviceConfig
{
public:

	enum MIDI_HANDLER_ID
	{
		NOTE_OFF,
		NOTE_ON,
		CONTROL_CHANGE,
		PROGRAM_CHANGE,
		PITCH_BEND,

		HANDLER_COUNT
	};

	USBMidi(USBHost &usb);

	virtual uint32_t Init(uint32_t parent, uint32_t port, uint32_t lowspeed);
	virtual uint32_t Release();
	virtual uint32_t Poll();
	virtual uint32_t GetAddress() { return _address; };


	void set_note_on_handler(void(*ptr)(byte channel, byte note, byte velocity)) { _note_on_handler = ptr; }
	void set_note_off_handler(void(*ptr)(byte channel, byte note, byte velocity)) { _note_off_handler = ptr; }
	void set_control_change_handler(void(*ptr)(byte channel, byte number, byte value)) { _control_change_handler = ptr; }
	void set_program_change_handler(void(*ptr)(byte channel, byte number)) { _program_change_handler = ptr; }
	void set_pitch_bend_handler(void(*ptr)(byte channel, int bend)) { _pitch_bend_handler = ptr; }

private:
	void attach_isr();
	void start_in_generation(uint32_t endpoint);
	void parse_config_descriptors(uint32_t address, uint8_t config_index, uint8_t &num_endpoints, uint8_t &conf_value);

	USBHost &_usb;
	uint32_t _address;
	

	/* Endpoint data structure */
	EpInfo  _ep_info[MIDI_MAX_ENDPOINTS];
	uint8_t _ringbuffer_data[RINGBUFFER_SIZE];

	// midi handlers
	void (*_note_on_handler)(byte, byte, byte);
	void(*_note_off_handler)(byte, byte, byte);
	void(*_control_change_handler)(byte, byte, byte);
	void(*_program_change_handler)(byte, byte);
	void(*_pitch_bend_handler)(byte, int);
};