#include <usb_midi.h>

USBHost Usb;
USBMidi Midi(Usb);


void midi_note_on(byte channel, byte note, byte velocity)
{
	printf("note on, chan: %d, note: %d, velocity: %d\r\n", channel, note, velocity);
}

void midi_note_off(byte channel, byte note, byte velocity)
{	
	printf("note off, chan: %d, note: %d\r\n", channel, note);
}

void midi_control_change(byte channel, byte number, byte value)
{
	printf("control change, chan: %d, number: %d, value: %d\r\n", channel, number, value);	
}

void midi_program_change(byte channel, byte number)
{	
	printf("program change, chan: %d, number: %d,\r\n", channel, number);	
}

void midi_pitch_bend(byte channel, int bend)
{
	printf("pitch bend, chan: %d, bend: %d,\r\n", channel, bend);
}

void raw_midi(uint32_t size, void *data)
{
	// Send entire packet to another midi device if used as USB to Midi converter
}

void setup()
{
	  Serial.begin(115200);
	  Midi.set_note_on_handler(midi_note_on);
	  Midi.set_note_off_handler(midi_note_off);
	  Midi.set_control_change_handler(midi_control_change);
	  Midi.set_program_change_handler(midi_program_change);
	  Midi.set_pitch_bend_handler(midi_pitch_bend);
	  
	  // Midi.set_raw_midi_handler(raw_midi);
}


void doDelay(unsigned long t1, unsigned long t2, unsigned long delayTime)
{
    unsigned long t3;

    if( t1 > t2 ){
      t3 = (4294967295 - t1 + t2);
    }else{
      t3 = t2 - t1;
    }

    if( t3 < delayTime ){
      delayMicroseconds(delayTime - t3);
    }
}

void loop()
{
	unsigned long t1 = micros();
	Usb.Task();
	doDelay(t1, micros(), 1000);
}