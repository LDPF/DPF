/*
 * DISTRHO Plugin Framework (DPF)
 * Copyright (C) 2012-2021 Filipe Coelho <falktx@falktx.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * For a full copy of the license see the LGPL.txt file
 */

#include "DistrhoPluginInternal.hpp"

#if DISTRHO_PLUGIN_HAS_UI
# include "DistrhoUIInternal.hpp"
# include "../extra/RingBuffer.hpp"
#else
# include "../extra/Sleep.hpp"
#endif

#include "jackbridge/JackBridge.cpp"
#include "lv2/lv2.h"

#ifndef DISTRHO_OS_WINDOWS
# include <signal.h>
#endif

#ifndef JACK_METADATA_ORDER
# define JACK_METADATA_ORDER "http://jackaudio.org/metadata/order"
#endif

#ifndef JACK_METADATA_PRETTY_NAME
# define JACK_METADATA_PRETTY_NAME "http://jackaudio.org/metadata/pretty-name"
#endif

#ifndef JACK_METADATA_PORT_GROUP
# define JACK_METADATA_PORT_GROUP "http://jackaudio.org/metadata/port-group"
#endif

#ifndef JACK_METADATA_SIGNAL_TYPE
# define JACK_METADATA_SIGNAL_TYPE "http://jackaudio.org/metadata/signal-type"
#endif

// -----------------------------------------------------------------------

START_NAMESPACE_DISTRHO

#if DISTRHO_PLUGIN_HAS_UI && ! DISTRHO_PLUGIN_WANT_MIDI_INPUT
static const sendNoteFunc sendNoteCallback = nullptr;
#endif
#if DISTRHO_PLUGIN_HAS_UI && ! DISTRHO_PLUGIN_WANT_STATE
static const setStateFunc setStateCallback = nullptr;
#endif
#if ! DISTRHO_PLUGIN_WANT_MIDI_OUTPUT
static const writeMidiFunc writeMidiCallback = nullptr;
#endif
#if ! DISTRHO_PLUGIN_WANT_PARAMETER_VALUE_CHANGE_REQUEST
static const requestParameterValueChangeFunc requestParameterValueChangeCallback = nullptr;
#endif

// -----------------------------------------------------------------------

static volatile bool gCloseSignalReceived = false;

#ifdef DISTRHO_OS_WINDOWS
static BOOL WINAPI winSignalHandler(DWORD dwCtrlType) noexcept
{
    if (dwCtrlType == CTRL_C_EVENT)
    {
        gCloseSignalReceived = true;
        return TRUE;
    }
    return FALSE;
}

static void initSignalHandler()
{
    SetConsoleCtrlHandler(winSignalHandler, TRUE);
}
#else
static void closeSignalHandler(int) noexcept
{
    gCloseSignalReceived = true;
}

static void initSignalHandler()
{
    struct sigaction sig;
    memset(&sig, 0, sizeof(sig));

    sig.sa_handler = closeSignalHandler;
    sig.sa_flags   = SA_RESTART;
    sigemptyset(&sig.sa_mask);
    sigaction(SIGINT, &sig, nullptr);
    sigaction(SIGTERM, &sig, nullptr);
}
#endif

// -----------------------------------------------------------------------

#if DISTRHO_PLUGIN_HAS_UI
class PluginJack : public DGL_NAMESPACE::IdleCallback
#else
class PluginJack
#endif
{
public:
    PluginJack(jack_client_t* const client)
        : fPlugin(this, writeMidiCallback, requestParameterValueChangeCallback),
#if DISTRHO_PLUGIN_HAS_UI
          fUI(this,
              0, // winId
              d_lastSampleRate,
              nullptr, // edit param
              setParameterValueCallback,
              setStateCallback,
              sendNoteCallback,
              nullptr, // window size
              nullptr, // file request
              nullptr, // bundle
              fPlugin.getInstancePointer(),
              0.0),
#endif
          fClient(client)
    {
#if DISTRHO_PLUGIN_NUM_INPUTS > 0 || DISTRHO_PLUGIN_NUM_OUTPUTS > 0
        char strBuf[0xff+1];
        strBuf[0xff] = '\0';

# if DISTRHO_PLUGIN_NUM_INPUTS > 0
        for (uint32_t i=0; i < DISTRHO_PLUGIN_NUM_INPUTS; ++i)
        {
            const AudioPort& port(fPlugin.getAudioPort(true, i));
            fPortAudioIns[i] = jackbridge_port_register(fClient, port.symbol, JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput, 0);
            setAudioPortMetadata(port, fPortAudioIns[i], i);
        }
# endif
# if DISTRHO_PLUGIN_NUM_OUTPUTS > 0
        for (uint32_t i=0; i < DISTRHO_PLUGIN_NUM_OUTPUTS; ++i)
        {
            std::snprintf(strBuf, 0xff, "out%i", i+1);
            const AudioPort& port(fPlugin.getAudioPort(false, i));
            fPortAudioOuts[i] = jackbridge_port_register(fClient, port.symbol, JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);
            setAudioPortMetadata(port, fPortAudioOuts[i], DISTRHO_PLUGIN_NUM_INPUTS+i);
        }
# endif
#endif

        fPortEventsIn = jackbridge_port_register(fClient, "events-in", JACK_DEFAULT_MIDI_TYPE, JackPortIsInput, 0);

#if DISTRHO_PLUGIN_WANT_MIDI_OUTPUT
        fPortMidiOut = jackbridge_port_register(fClient, "midi-out", JACK_DEFAULT_MIDI_TYPE, JackPortIsOutput, 0);
        fPortMidiOutBuffer = nullptr;
#endif

#if DISTRHO_PLUGIN_WANT_PROGRAMS
        if (fPlugin.getProgramCount() > 0)
        {
            fPlugin.loadProgram(0);
# if DISTRHO_PLUGIN_HAS_UI
            fUI.programLoaded(0);
# endif
        }
# if DISTRHO_PLUGIN_HAS_UI
        fProgramChanged = -1;
# endif
#endif

        if (const uint32_t count = fPlugin.getParameterCount())
        {
            fLastOutputValues = new float[count];
            std::memset(fLastOutputValues, 0, sizeof(float)*count);

#if DISTRHO_PLUGIN_HAS_UI
            fParametersChanged = new bool[count];
            std::memset(fParametersChanged, 0, sizeof(bool)*count);
#endif

            for (uint32_t i=0; i < count; ++i)
            {
#if DISTRHO_PLUGIN_HAS_UI
                if (! fPlugin.isParameterOutput(i))
                    fUI.parameterChanged(i, fPlugin.getParameterValue(i));
#endif
            }
        }
        else
        {
            fLastOutputValues = nullptr;
#if DISTRHO_PLUGIN_HAS_UI
            fParametersChanged = nullptr;
#endif
        }

        jackbridge_set_buffer_size_callback(fClient, jackBufferSizeCallback, this);
        jackbridge_set_sample_rate_callback(fClient, jackSampleRateCallback, this);
        jackbridge_set_process_callback(fClient, jackProcessCallback, this);
        jackbridge_on_shutdown(fClient, jackShutdownCallback, this);

        fPlugin.activate();

        jackbridge_activate(fClient);

        std::fflush(stdout);

#if DISTRHO_PLUGIN_HAS_UI
        if (const char* const name = jackbridge_get_client_name(fClient))
            fUI.setWindowTitle(name);
        else
            fUI.setWindowTitle(fPlugin.getName());

        fUI.exec(this);
#else
        while (! gCloseSignalReceived)
            d_sleep(1);
#endif
    }

    ~PluginJack()
    {
        if (fClient != nullptr)
            jackbridge_deactivate(fClient);

        if (fLastOutputValues != nullptr)
        {
            delete[] fLastOutputValues;
            fLastOutputValues = nullptr;
        }

#if DISTRHO_PLUGIN_HAS_UI
        if (fParametersChanged != nullptr)
        {
            delete[] fParametersChanged;
            fParametersChanged = nullptr;
        }
#endif

        fPlugin.deactivate();

        if (fClient == nullptr)
            return;

#if DISTRHO_PLUGIN_WANT_MIDI_OUTPUT
        jackbridge_port_unregister(fClient, fPortMidiOut);
        fPortMidiOut = nullptr;
#endif

        jackbridge_port_unregister(fClient, fPortEventsIn);
        fPortEventsIn = nullptr;

#if DISTRHO_PLUGIN_NUM_INPUTS > 0
        for (uint32_t i=0; i < DISTRHO_PLUGIN_NUM_INPUTS; ++i)
        {
            jackbridge_port_unregister(fClient, fPortAudioIns[i]);
            fPortAudioIns[i] = nullptr;
        }
#endif

#if DISTRHO_PLUGIN_NUM_OUTPUTS > 0
        for (uint32_t i=0; i < DISTRHO_PLUGIN_NUM_OUTPUTS; ++i)
        {
            jackbridge_port_unregister(fClient, fPortAudioOuts[i]);
            fPortAudioOuts[i] = nullptr;
        }
#endif

        jackbridge_client_close(fClient);
    }

    // -------------------------------------------------------------------

protected:
#if DISTRHO_PLUGIN_HAS_UI
    void idleCallback() override
    {
        if (gCloseSignalReceived)
            return fUI.quit();

# if DISTRHO_PLUGIN_WANT_PROGRAMS
        if (fProgramChanged >= 0)
        {
            fUI.programLoaded(fProgramChanged);
            fProgramChanged = -1;
        }
# endif

        for (uint32_t i=0, count=fPlugin.getParameterCount(); i < count; ++i)
        {
            if (fPlugin.isParameterOutput(i))
            {
                const float value = fPlugin.getParameterValue(i);

                if (d_isEqual(fLastOutputValues[i], value))
                    continue;

                fLastOutputValues[i] = value;
                fUI.parameterChanged(i, value);
            }
            else if (fParametersChanged[i])
            {
                fParametersChanged[i] = false;
                fUI.parameterChanged(i, fPlugin.getParameterValue(i));
            }
        }

        fUI.exec_idle();
    }
#endif

    void jackBufferSize(const jack_nframes_t nframes)
    {
        fPlugin.setBufferSize(nframes, true);
    }

    void jackSampleRate(const jack_nframes_t nframes)
    {
        fPlugin.setSampleRate(nframes, true);
    }

    void jackProcess(const jack_nframes_t nframes)
    {
#if DISTRHO_PLUGIN_NUM_INPUTS > 0
        const float* audioIns[DISTRHO_PLUGIN_NUM_INPUTS];

        for (uint32_t i=0; i < DISTRHO_PLUGIN_NUM_INPUTS; ++i)
            audioIns[i] = (const float*)jackbridge_port_get_buffer(fPortAudioIns[i], nframes);
#else
        static const float** audioIns = nullptr;
#endif

#if DISTRHO_PLUGIN_NUM_OUTPUTS > 0
        float* audioOuts[DISTRHO_PLUGIN_NUM_OUTPUTS];

        for (uint32_t i=0; i < DISTRHO_PLUGIN_NUM_OUTPUTS; ++i)
            audioOuts[i] = (float*)jackbridge_port_get_buffer(fPortAudioOuts[i], nframes);
#else
        static float** audioOuts = nullptr;
#endif

#if DISTRHO_PLUGIN_WANT_TIMEPOS
        jack_position_t pos;
        fTimePosition.playing = (jackbridge_transport_query(fClient, &pos) == JackTransportRolling);

        if (pos.unique_1 == pos.unique_2)
        {
            fTimePosition.frame = pos.frame;

            if (pos.valid & JackPositionBBT)
            {
                fTimePosition.bbt.valid = true;

                fTimePosition.bbt.bar  = pos.bar;
                fTimePosition.bbt.beat = pos.beat;
                fTimePosition.bbt.tick = pos.tick;
#ifdef JACK_TICK_DOUBLE
                if (pos.valid & JackTickDouble)
                    fTimePosition.bbt.tick = pos.tick_double;
                else
#endif
                    fTimePosition.bbt.tick = pos.tick;
                fTimePosition.bbt.barStartTick = pos.bar_start_tick;

                fTimePosition.bbt.beatsPerBar = pos.beats_per_bar;
                fTimePosition.bbt.beatType    = pos.beat_type;

                fTimePosition.bbt.ticksPerBeat   = pos.ticks_per_beat;
                fTimePosition.bbt.beatsPerMinute = pos.beats_per_minute;
            }
            else
                fTimePosition.bbt.valid = false;
        }
        else
        {
            fTimePosition.bbt.valid = false;
            fTimePosition.frame = 0;
        }

        fPlugin.setTimePosition(fTimePosition);
#endif

        updateParameterTriggers();

#if DISTRHO_PLUGIN_WANT_MIDI_OUTPUT
        fPortMidiOutBuffer = jackbridge_port_get_buffer(fPortMidiOut, nframes);
        jackbridge_midi_clear_buffer(fPortMidiOutBuffer);
#endif

#if DISTRHO_PLUGIN_WANT_MIDI_INPUT
        uint32_t  midiEventCount = 0;
        MidiEvent midiEvents[512];

# if DISTRHO_PLUGIN_HAS_UI
        while (fNotesRingBuffer.isDataAvailableForReading())
        {
            uint8_t midiData[3];
            if (! fNotesRingBuffer.readCustomData(midiData, 3))
                break;

            MidiEvent& midiEvent(midiEvents[midiEventCount++]);
            midiEvent.frame = 0;
            midiEvent.size  = 3;
            std::memcpy(midiEvent.data, midiData, 3);

            if (midiEventCount == 512)
                break;
        }
# endif
#else
        static const uint32_t midiEventCount = 0;
#endif

        void* const midiInBuf = jackbridge_port_get_buffer(fPortEventsIn, nframes);

        if (const uint32_t eventCount = std::min(512u - midiEventCount, jackbridge_midi_get_event_count(midiInBuf)))
        {
            jack_midi_event_t jevent;

            for (uint32_t i=0; i < eventCount; ++i)
            {
                if (jackbridge_midi_event_get(&jevent, midiInBuf, i) != 0)
                    break;

                // Check if message is control change on channel 1
                if (jevent.buffer[0] == 0xB0 && jevent.size == 3)
                {
                    const uint8_t control = jevent.buffer[1];
                    const uint8_t value   = jevent.buffer[2];

                    /* NOTE: This is not optimal, we're iterating all parameters on every CC message.
                             Since the JACK standalone is more of a test tool, this will do for now. */
                    for (uint32_t j=0, paramCount=fPlugin.getParameterCount(); j < paramCount; ++j)
                    {
                        if (fPlugin.isParameterOutput(j))
                            continue;
                        if (fPlugin.getParameterMidiCC(j) != control)
                            continue;

                        const float scaled = static_cast<float>(value)/127.0f;
                        const float fvalue = fPlugin.getParameterRanges(j).getUnnormalizedValue(scaled);
                        fPlugin.setParameterValue(j, fvalue);
#if DISTRHO_PLUGIN_HAS_UI
                        fParametersChanged[j] = true;
#endif
                        break;
                    }
                }
#if DISTRHO_PLUGIN_WANT_PROGRAMS
                // Check if message is program change on channel 1
                else if (jevent.buffer[0] == 0xC0 && jevent.size == 2)
                {
                    const uint8_t program = jevent.buffer[1];

                    if (program < fPlugin.getProgramCount())
                    {
                        fPlugin.loadProgram(program);
# if DISTRHO_PLUGIN_HAS_UI
                        fProgramChanged = program;
# endif
                    }
                }
#endif

#if DISTRHO_PLUGIN_WANT_MIDI_INPUT
                MidiEvent& midiEvent(midiEvents[midiEventCount++]);

                midiEvent.frame = jevent.time;
                midiEvent.size  = jevent.size;

                if (midiEvent.size > MidiEvent::kDataSize)
                    midiEvent.dataExt = jevent.buffer;
                else
                    std::memcpy(midiEvent.data, jevent.buffer, midiEvent.size);
#endif
            }
        }

#if DISTRHO_PLUGIN_WANT_MIDI_INPUT
        fPlugin.run(audioIns, audioOuts, nframes, midiEvents, midiEventCount);
#else
        fPlugin.run(audioIns, audioOuts, nframes);
#endif

#if DISTRHO_PLUGIN_WANT_MIDI_OUTPUT
        fPortMidiOutBuffer = nullptr;
#endif
    }

    void jackShutdown()
    {
        d_stderr("jack has shutdown, quitting now...");
        fClient = nullptr;
#if DISTRHO_PLUGIN_HAS_UI
        fUI.quit();
#endif
    }

    // -------------------------------------------------------------------

#if DISTRHO_PLUGIN_HAS_UI
    void setParameterValue(const uint32_t index, const float value)
    {
        fPlugin.setParameterValue(index, value);
    }

# if DISTRHO_PLUGIN_WANT_MIDI_INPUT
    void sendNote(const uint8_t channel, const uint8_t note, const uint8_t velocity)
    {
        uint8_t midiData[3];
        midiData[0] = (velocity != 0 ? 0x90 : 0x80) | channel;
        midiData[1] = note;
        midiData[2] = velocity;
        fNotesRingBuffer.writeCustomData(midiData, 3);
        fNotesRingBuffer.commitWrite();
    }
# endif

# if DISTRHO_PLUGIN_WANT_STATE
    void setState(const char* const key, const char* const value)
    {
        fPlugin.setState(key, value);
    }
# endif
#endif // DISTRHO_PLUGIN_HAS_UI

    // NOTE: no trigger support for JACK, simulate it here
    void updateParameterTriggers()
    {
        float defValue;

        for (uint32_t i=0, count=fPlugin.getParameterCount(); i < count; ++i)
        {
            if ((fPlugin.getParameterHints(i) & kParameterIsTrigger) != kParameterIsTrigger)
                continue;

            defValue = fPlugin.getParameterRanges(i).def;

            if (d_isNotEqual(defValue, fPlugin.getParameterValue(i)))
                fPlugin.setParameterValue(i, defValue);
        }
    }

    // -------------------------------------------------------------------

private:
    PluginExporter fPlugin;
#if DISTRHO_PLUGIN_HAS_UI
    UIExporter     fUI;
#endif

    jack_client_t* fClient;

#if DISTRHO_PLUGIN_NUM_INPUTS > 0
    jack_port_t* fPortAudioIns[DISTRHO_PLUGIN_NUM_INPUTS];
#endif
#if DISTRHO_PLUGIN_NUM_OUTPUTS > 0
    jack_port_t* fPortAudioOuts[DISTRHO_PLUGIN_NUM_OUTPUTS];
#endif
    jack_port_t* fPortEventsIn;
#if DISTRHO_PLUGIN_WANT_MIDI_OUTPUT
    jack_port_t* fPortMidiOut;
    void*        fPortMidiOutBuffer;
#endif
#if DISTRHO_PLUGIN_WANT_TIMEPOS
    TimePosition fTimePosition;
#endif

    // Temporary data
    float* fLastOutputValues;

#if DISTRHO_PLUGIN_HAS_UI
    // Store DSP changes to send to UI
    bool* fParametersChanged;
# if DISTRHO_PLUGIN_WANT_PROGRAMS
    int fProgramChanged;
# endif
# if DISTRHO_PLUGIN_WANT_MIDI_INPUT
    SmallStackRingBuffer fNotesRingBuffer;
# endif
#endif

    void setAudioPortMetadata(const AudioPort& port, jack_port_t* const jackport, const uint32_t index)
    {
        DISTRHO_SAFE_ASSERT_RETURN(jackport != nullptr,);

        const jack_uuid_t uuid = jackbridge_port_uuid(jackport);

        if (uuid == JACK_UUID_EMPTY_INITIALIZER)
            return;

        jackbridge_set_property(fClient, uuid, JACK_METADATA_PRETTY_NAME, port.name, "text/plain");

        {
            char strBuf[0xff];
            snprintf(strBuf, sizeof(0xff)-1, "%u", index);
            jackbridge_set_property(fClient, uuid, JACK_METADATA_ORDER, strBuf, "http://www.w3.org/2001/XMLSchema#integer");
        }

        if (port.groupId != kPortGroupNone)
        {
            const PortGroupWithId& portGroup(fPlugin.getPortGroupById(port.groupId));
            jackbridge_set_property(fClient, uuid, JACK_METADATA_PORT_GROUP, portGroup.name, "text/plain");
        }

        if (port.hints & kAudioPortIsCV)
        {
            jackbridge_set_property(fClient, uuid, JACK_METADATA_SIGNAL_TYPE, "CV", "text/plain");
        }
        else
        {
            jackbridge_set_property(fClient, uuid, JACK_METADATA_SIGNAL_TYPE, "AUDIO", "text/plain");
            return;
        }

        // set cv ranges
        const bool cvPortScaled = port.hints & kCVPortHasScaledRange;

        if (port.hints & kCVPortHasBipolarRange)
        {
            if (cvPortScaled)
            {
                jackbridge_set_property(fClient, uuid, LV2_CORE__minimum, "-5", "http://www.w3.org/2001/XMLSchema#integer");
                jackbridge_set_property(fClient, uuid, LV2_CORE__maximum, "5", "http://www.w3.org/2001/XMLSchema#integer");
            }
            else
            {
                jackbridge_set_property(fClient, uuid, LV2_CORE__minimum, "-1", "http://www.w3.org/2001/XMLSchema#integer");
                jackbridge_set_property(fClient, uuid, LV2_CORE__maximum, "1", "http://www.w3.org/2001/XMLSchema#integer");
            }
        }
        else if (port.hints & kCVPortHasNegativeUnipolarRange)
        {
            if (cvPortScaled)
            {
                jackbridge_set_property(fClient, uuid, LV2_CORE__minimum, "-10", "http://www.w3.org/2001/XMLSchema#integer");
                jackbridge_set_property(fClient, uuid, LV2_CORE__maximum, "0", "http://www.w3.org/2001/XMLSchema#integer");
            }
            else
            {
                jackbridge_set_property(fClient, uuid, LV2_CORE__minimum, "-1", "http://www.w3.org/2001/XMLSchema#integer");
                jackbridge_set_property(fClient, uuid, LV2_CORE__maximum, "0", "http://www.w3.org/2001/XMLSchema#integer");
            }
        }
        else if (port.hints & kCVPortHasPositiveUnipolarRange)
        {
            if (cvPortScaled)
            {
                jackbridge_set_property(fClient, uuid, LV2_CORE__minimum, "0", "http://www.w3.org/2001/XMLSchema#integer");
                jackbridge_set_property(fClient, uuid, LV2_CORE__maximum, "10", "http://www.w3.org/2001/XMLSchema#integer");
            }
            else
            {
                jackbridge_set_property(fClient, uuid, LV2_CORE__minimum, "0", "http://www.w3.org/2001/XMLSchema#integer");
                jackbridge_set_property(fClient, uuid, LV2_CORE__maximum, "1", "http://www.w3.org/2001/XMLSchema#integer");
            }
        }
    }

    // -------------------------------------------------------------------
    // Callbacks

    #define thisPtr ((PluginJack*)ptr)

    static int jackBufferSizeCallback(jack_nframes_t nframes, void* ptr)
    {
        thisPtr->jackBufferSize(nframes);
        return 0;
    }

    static int jackSampleRateCallback(jack_nframes_t nframes, void* ptr)
    {
        thisPtr->jackSampleRate(nframes);
        return 0;
    }

    static int jackProcessCallback(jack_nframes_t nframes, void* ptr)
    {
        thisPtr->jackProcess(nframes);
        return 0;
    }

    static void jackShutdownCallback(void* ptr)
    {
        thisPtr->jackShutdown();
    }

#if DISTRHO_PLUGIN_HAS_UI
    static void setParameterValueCallback(void* ptr, uint32_t index, float value)
    {
        thisPtr->setParameterValue(index, value);
    }

# if DISTRHO_PLUGIN_WANT_MIDI_INPUT
    static void sendNoteCallback(void* ptr, uint8_t channel, uint8_t note, uint8_t velocity)
    {
        thisPtr->sendNote(channel, note, velocity);
    }
# endif

# if DISTRHO_PLUGIN_WANT_STATE
    static void setStateCallback(void* ptr, const char* key, const char* value)
    {
        thisPtr->setState(key, value);
    }
# endif
#endif // DISTRHO_PLUGIN_HAS_UI

#if DISTRHO_PLUGIN_WANT_PARAMETER_VALUE_CHANGE_REQUEST
    bool requestParameterValueChange(const uint32_t index, const float value)
    {
        DISTRHO_SAFE_ASSERT_RETURN(index < fPlugin.getParameterCount(), false);

        fPlugin.setParameterValue(index, value);
# if DISTRHO_PLUGIN_HAS_UI
        fParametersChanged[index] = true;
# endif
        return true;
    }

    static bool requestParameterValueChangeCallback(void* ptr, const uint32_t index, const float value)
    {
        return thisPtr->requestParameterValueChange(index, value);
    }
#endif

#if DISTRHO_PLUGIN_WANT_MIDI_OUTPUT
    bool writeMidi(const MidiEvent& midiEvent)
    {
        DISTRHO_SAFE_ASSERT_RETURN(fPortMidiOutBuffer != nullptr, false);

        return jackbridge_midi_event_write(fPortMidiOutBuffer,
                                           midiEvent.frame,
                                           midiEvent.size > MidiEvent::kDataSize ? midiEvent.dataExt : midiEvent.data,
                                           midiEvent.size) == 0;
    }

    static bool writeMidiCallback(void* ptr, const MidiEvent& midiEvent)
    {
        return thisPtr->writeMidi(midiEvent);
    }
#endif

    #undef thisPtr
};

END_NAMESPACE_DISTRHO

// -----------------------------------------------------------------------

int main()
{
    USE_NAMESPACE_DISTRHO;

    jack_status_t  status = jack_status_t(0x0);
    jack_client_t* client = jackbridge_client_open(DISTRHO_PLUGIN_NAME, JackNoStartServer, &status);

    if (client == nullptr)
    {
        String errorString;

        if (status & JackFailure)
            errorString += "Overall operation failed;\n";
        if (status & JackInvalidOption)
            errorString += "The operation contained an invalid or unsupported option;\n";
        if (status & JackNameNotUnique)
            errorString += "The desired client name was not unique;\n";
        if (status & JackServerStarted)
            errorString += "The JACK server was started as a result of this operation;\n";
        if (status & JackServerFailed)
            errorString += "Unable to connect to the JACK server;\n";
        if (status & JackServerError)
            errorString += "Communication error with the JACK server;\n";
        if (status & JackNoSuchClient)
            errorString += "Requested client does not exist;\n";
        if (status & JackLoadFailure)
            errorString += "Unable to load internal client;\n";
        if (status & JackInitFailure)
            errorString += "Unable to initialize client;\n";
        if (status & JackShmFailure)
            errorString += "Unable to access shared memory;\n";
        if (status & JackVersionError)
            errorString += "Client's protocol version does not match;\n";
        if (status & JackBackendError)
            errorString += "Backend Error;\n";
        if (status & JackClientZombie)
            errorString += "Client is being shutdown against its will;\n";

        if (errorString.isNotEmpty())
        {
            errorString[errorString.length()-2] = '.';
            d_stderr("Failed to create jack client, reason was:\n%s", errorString.buffer());
        }
        else
            d_stderr("Failed to create jack client, cannot continue!");

        return 1;
    }

    USE_NAMESPACE_DISTRHO;

    initSignalHandler();

    d_lastBufferSize = jackbridge_get_buffer_size(client);
    d_lastSampleRate = jackbridge_get_sample_rate(client);
    d_lastCanRequestParameterValueChanges = true;

    const PluginJack p(client);

    return 0;
}

// -----------------------------------------------------------------------
