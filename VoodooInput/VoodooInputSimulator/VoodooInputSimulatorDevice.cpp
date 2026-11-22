//
//  VoodooInputSimulatorDevice.cpp
//  VoodooInput
//
//  Copyright © 2018 Alexandre Daoud and Kishor Prins. All rights reserved.
//

#include "VoodooInput.hpp"
#include "VoodooInputSimulatorDevice.hpp"
#include "../VoodooInputMultitouch/MultitouchHelpers.h"
#include "VoodooInputIDs.hpp"

#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOCommandGate.h>

#define super IOHIDDevice
OSDefineMetaClassAndStructors(VoodooInputSimulatorDevice, IOHIDDevice);


const unsigned char report_descriptor[] = {0x05, 0x01, 0x09, 0x02, 0xa1, 0x01, 0x09, 0x01, 0xa1, 0x00, 0x05, 0x09, 0x19, 0x01, 0x29, 0x03, 0x15, 0x00, 0x25, 0x01, 0x85, 0x02, 0x95, 0x03, 0x75, 0x01, 0x81, 0x02, 0x95, 0x01, 0x75, 0x05, 0x81, 0x01, 0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x15, 0x81, 0x25, 0x7f, 0x75, 0x08, 0x95, 0x02, 0x81, 0x06, 0x95, 0x04, 0x75, 0x08, 0x81, 0x01, 0xc0, 0xc0, 0x05, 0x0d, 0x09, 0x05, 0xa1, 0x01, 0x06, 0x00, 0xff, 0x09, 0x0c, 0x15, 0x00, 0x26, 0xff, 0x00, 0x75, 0x08, 0x95, 0x10, 0x85, 0x3f, 0x81, 0x22, 0xc0, 0x06, 0x00, 0xff, 0x09, 0x0c, 0xa1, 0x01, 0x06, 0x00, 0xff, 0x09, 0x0c, 0x15, 0x00, 0x26, 0xff, 0x00, 0x85, 0x44, 0x75, 0x08, 0x96, 0x6b, 0x05, 0x81, 0x00, 0xc0};

void VoodooInputSimulatorDevice::constructReport(const VoodooInputEvent& multitouch_event) {
    if (!ready_for_reports)
        return;

    command_gate->runAction(OSMemberFunctionCast(IOCommandGate::Action, this, &VoodooInputSimulatorDevice::constructReportGated), (void*)&multitouch_event);
}

void VoodooInputSimulatorDevice::sendReport() {
#if DEBUG
    size_t fingerCount = (input_report_buffer->getLength() - sizeof(MAGIC_TRACKPAD_INPUT_REPORT)) / sizeof(MAGIC_TRACKPAD_INPUT_REPORT_FINGER);
    IOLog("Sending report with touch active %d, button %d, finger count %zu\n", input_report->TouchActive, input_report->Button, fingerCount);
    for (size_t i = 0; i < fingerCount; i++) {
        MAGIC_TRACKPAD_INPUT_REPORT_FINGER &f = input_report->FINGERS[i];
        IOLog("[%zu] (%d, %d) F%d St%d Maj%d Min%d Sz%d P%d ID%d A%d\n", i, f.X, f.Y, f.Finger, f.State, f.Touch_Major, f.Touch_Minor, f.Size, f.Pressure, f.Identifier, f.Angle);
    }
#endif
    handleReport(input_report_buffer, kIOHIDReportTypeInput);
}

void VoodooInputSimulatorDevice::constructReportGated(const VoodooInputEvent& multitouch_event) {
    AbsoluteTime timestamp = multitouch_event.timestamp;

    input_report->ReportID = 0x02;
    input_report->Unused[0] = 0;
    input_report->Unused[1] = 0;
    input_report->Unused[2] = 0;
    input_report->Unused[3] = 0;
    input_report->Unused[4] = 0;

    const VoodooInputTransducer* transducer = &multitouch_event.transducers[0];

    // physical button
    bool physical_button = transducer->isPhysicalButtonDown;
    input_report->Button = physical_button;

    // rotation check
    UInt8 transform = engine->getTransformKey();

    // multitouch report id
    input_report->multitouch_report_id = 0x31; // Magic

    // timestamp
    AbsoluteTime relative_timestamp = timestamp;
    SUB_ABSOLUTETIME(&relative_timestamp, &start_timestamp);

    UInt64 milli_timestamp;
    absolutetime_to_nanoseconds(relative_timestamp, &milli_timestamp);
    milli_timestamp /= 1000000;

    input_report->timestamp_buffer[0] = (milli_timestamp << 0x3) | 0x4;
    input_report->timestamp_buffer[1] = (milli_timestamp >> 0x5) & 0xFF;
    input_report->timestamp_buffer[2] = (milli_timestamp >> 0xd) & 0xFF;

    // finger data
    bool fingers_active = false;
    bool is_error_input_active = false;
    int active_finger_count = 0;
    int first_active_idx = -1;
    SInt32 first_active_x = 0;
    SInt32 first_active_y = 0;

    for (int i = 0; i < multitouch_event.contact_count; i++) {
        const VoodooInputTransducer* transducer = &multitouch_event.transducers[i];

        if (!transducer || !transducer->isValid)
            continue;

        if (transducer->type == VoodooInputTransducerType::STYLUS) {
            continue;
        }

        // in case the obtained id is greater than 14, usually 0~4 for common devices.
        UInt16 touch_id = transducer->secondaryId % 15;
        fingers_active |= transducer->isTransducerActive;

        MAGIC_TRACKPAD_INPUT_REPORT_FINGER& finger_data = input_report->FINGERS[i];

        IOFixed scaled_x = ((transducer->currentCoordinates.x * 1.0f) / engine->getLogicalMaxX()) * MT2_MAX_X;
        IOFixed scaled_y = ((transducer->currentCoordinates.y * 1.0f) / engine->getLogicalMaxY()) * MT2_MAX_Y;

        if (scaled_x < 1 && scaled_y >= MT2_MAX_Y) {
            is_error_input_active = true;
        }

        if (transform) {
            if (transform & kIOFBSwapAxes) {
                scaled_x = ((transducer->currentCoordinates.y * 1.0f) / engine->getLogicalMaxY()) * MT2_MAX_X;
                scaled_y = ((transducer->currentCoordinates.x * 1.0f) / engine->getLogicalMaxX()) * MT2_MAX_Y;
            }

            if (transform & kIOFBInvertX) {
                scaled_x = MT2_MAX_X - scaled_x;
            }
            if (transform & kIOFBInvertY) {
                scaled_y = MT2_MAX_Y - scaled_y;
            }
        }

        if (transducer->isTransducerActive && transducer->fingerType != kMT2FingerTypePalm) {
            if (first_active_idx < 0) {
                first_active_idx = i;
                first_active_x = (SInt32)scaled_x;
                first_active_y = (SInt32)scaled_y;
            }
            active_finger_count++;
        }

        finger_data.State = touch_active[touch_id] ? kTouchStateActive : kTouchStateStart;
        touch_active[touch_id] = transducer->isTransducerActive || transducer->isPhysicalButtonDown;

        finger_data.Finger = transducer->fingerType;

        if (transducer->supportsPressure) {
            finger_data.Pressure = transducer->currentCoordinates.pressure;
            finger_data.Size = transducer->currentCoordinates.width;
            finger_data.Touch_Major = transducer->currentCoordinates.width;
            finger_data.Touch_Minor = transducer->currentCoordinates.width;
        } else {
            finger_data.Pressure = 5;
            finger_data.Size = 10;
            finger_data.Touch_Major = 20;
            finger_data.Touch_Minor = 20;
        }

        if (physical_button) {
            finger_data.Pressure = 120;
        }

        if (!transducer->isTransducerActive && !transducer->isPhysicalButtonDown) {
            finger_data.State = kTouchStateStop;
            finger_data.Size = 0x0;
            finger_data.Pressure = 0x0;
            finger_data.Touch_Minor = 0;
            finger_data.Touch_Major = 0;
        }

        finger_data.X = (SInt16)(scaled_x - (MT2_MAX_X / 2));
        finger_data.Y = (SInt16)(scaled_y - (MT2_MAX_Y / 2)) * -1;

        finger_data.Angle = 0x4; // pi/2
        finger_data.Identifier = touch_id + 1;
    }

    // Unified libinput-style tap-to-click + double-tap-drag + 2F right-click
    // Requires macOS "Tap to click" = OFF and "Use trackpad for dragging" = OFF
    constexpr UInt64 kTapMaxDurationMs = 190;         // libinput DEFAULT_TAP_TIMEOUT_PERIOD (180 ms)
    constexpr UInt32 kTappedHoldMs = 180;             // libinput drag gap (180 ms)
    constexpr UInt64 kDragHoldMs = 70;                // hold threshold on Tap #2 to lock drag
    constexpr UInt32 kEdgeDragWaitMs = 300;           // libinput DEFAULT_DRAGLOCK_TIMEOUT_PERIOD
    constexpr SInt32 kTapMaxMoveSq = 95 * 95;         // ~1.4 mm (libinput DEFAULT_TAP_MOVE_THRESHOLD 1.3 mm)
    constexpr SInt32 kTwoFingerMaxMoveSq = 180 * 180; // ~2.6 mm for 2-finger right-click tap
    constexpr SInt32 kTapPairMaxDistSq = 420 * 420;   // ~6 mm: farther touch finishes single-click immediately
    constexpr SInt32 kDragStartMoveSq = 30 * 30;      // ~0.45 mm instant drag grab
    constexpr SInt32 kEdgeMarginX = 800;              // ~10% edge continuation border
    constexpr SInt32 kEdgeMarginY = 520;

    bool hold_virtual_button = false;
    int synth_finger_count = 0;
    bool need_pre_release = false;

    if (physical_button || active_finger_count >= 3) {
        if (tap_timer) tap_timer->cancelTimeout();
        if (tap_state == kTapTapped || tap_state == kTapSecondPending ||
            tap_state == kTapDragging || tap_state == kTapDragging2 ||
            tap_state == kTapDraggingWait || tap_state == kTapRightClickPending) {
            need_pre_release = true;
        }
        tap_state = kTapIdle;
    } else if (active_finger_count == 2) {
        if (tap_state == kTapDragging || tap_state == kTapDragging2) {
            tap_state = kTapDragging2;
            last_drag_x = first_active_x;
            last_drag_y = first_active_y;
        } else if (tap_state == kTapIdle || tap_state == kTapFirstDown) {
            if (tap_state == kTapIdle) {
                tap_down_ms = milli_timestamp;
                tap_down_x = first_active_x;
                tap_down_y = first_active_y;
                tap_max_dist_sq = 0;
            }
            tap_state = kTapTwoFingerDown;
        } else if (tap_state == kTapTwoFingerDown) {
            SInt32 dx = first_active_x - tap_down_x;
            SInt32 dy = first_active_y - tap_down_y;
            SInt32 dsq = dx * dx + dy * dy;
            if (dsq > tap_max_dist_sq)
                tap_max_dist_sq = dsq;
        } else {
            if (tap_timer) tap_timer->cancelTimeout();
            if (tap_state == kTapTapped || tap_state == kTapSecondPending || tap_state == kTapDraggingWait) {
                need_pre_release = true;
            }
            tap_state = kTapIdle;
        }
    } else if (active_finger_count == 1) {
        switch (tap_state) {
            case kTapIdle:
                tap_state = kTapFirstDown;
                tap_down_ms = milli_timestamp;
                tap_down_x = first_active_x;
                tap_down_y = first_active_y;
                tap_max_dist_sq = 0;
                break;

            case kTapFirstDown:
            case kTapTwoFingerDown: {
                SInt32 dx = first_active_x - tap_down_x;
                SInt32 dy = first_active_y - tap_down_y;
                SInt32 dsq = dx * dx + dy * dy;
                if (dsq > tap_max_dist_sq)
                    tap_max_dist_sq = dsq;
                break;
            }

            case kTapTapped: {
                if (tap_timer) tap_timer->cancelTimeout();
                SInt32 dx = first_active_x - tap_up_x;
                SInt32 dy = first_active_y - tap_up_y;
                SInt32 dsq = dx * dx + dy * dy;
                bool within_time = (milli_timestamp >= tap_up_ms) && ((milli_timestamp - tap_up_ms) <= (UInt64)kTappedHoldMs);
                if (within_time && dsq <= kTapPairMaxDistSq) {
                    tap_state = kTapSecondPending;
                    tap_down_ms = milli_timestamp;
                    tap_down_x = first_active_x;
                    tap_down_y = first_active_y;
                } else {
                    need_pre_release = true;
                    tap_state = kTapFirstDown;
                    tap_down_ms = milli_timestamp;
                    tap_down_x = first_active_x;
                    tap_down_y = first_active_y;
                    tap_max_dist_sq = 0;
                }
                break;
            }

            case kTapSecondPending: {
                SInt32 dx = first_active_x - tap_down_x;
                SInt32 dy = first_active_y - tap_down_y;
                SInt32 dsq = dx * dx + dy * dy;
                bool held_enough = (milli_timestamp >= tap_down_ms) && ((milli_timestamp - tap_down_ms) >= kDragHoldMs);
                if (dsq >= kDragStartMoveSq || held_enough) {
                    tap_state = kTapDragging;
                    last_drag_x = first_active_x;
                    last_drag_y = first_active_y;
                }
                break;
            }

            case kTapDraggingWait:
                if (tap_timer) tap_timer->cancelTimeout();
                tap_state = kTapDragging;
                last_drag_x = first_active_x;
                last_drag_y = first_active_y;
                break;

            case kTapDragging2:
                tap_state = kTapDragging;
                last_drag_x = first_active_x;
                last_drag_y = first_active_y;
                break;

            case kTapDragging:
                last_drag_x = first_active_x;
                last_drag_y = first_active_y;
                break;

            case kTapRightClickPending:
                if (tap_timer) tap_timer->cancelTimeout();
                need_pre_release = true;
                tap_state = kTapFirstDown;
                tap_down_ms = milli_timestamp;
                tap_down_x = first_active_x;
                tap_down_y = first_active_y;
                tap_max_dist_sq = 0;
                break;
        }
    } else {
        // Zero active fingers (lift-off)
        if (tap_state == kTapFirstDown) {
            bool quick_tap = (milli_timestamp >= tap_down_ms) && ((milli_timestamp - tap_down_ms) <= kTapMaxDurationMs);
            if (quick_tap && tap_max_dist_sq <= kTapMaxMoveSq && tap_timer) {
                tap_state = kTapTapped;
                tap_up_ms = milli_timestamp;
                tap_up_x = tap_down_x;
                tap_up_y = tap_down_y;
                hold_virtual_button = true;
                synth_finger_count = 1;
                tap_timer->setTimeoutMS(kTappedHoldMs);
            } else {
                tap_state = kTapIdle;
            }
        } else if (tap_state == kTapTwoFingerDown) {
            bool quick_tap = (milli_timestamp >= tap_down_ms) && ((milli_timestamp - tap_down_ms) <= kTapMaxDurationMs);
            if (quick_tap && tap_max_dist_sq <= kTwoFingerMaxMoveSq && tap_timer) {
                tap_state = kTapRightClickPending;
                tap_up_x = tap_down_x;
                tap_up_y = tap_down_y;
                hold_virtual_button = true;
                synth_finger_count = 2;
                tap_timer->setTimeoutMS(30);
            } else {
                tap_state = kTapIdle;
            }
        } else if (tap_state == kTapSecondPending) {
            // Quick Tap #2 lift -> finish Click #1, start Click #2 (libinput tap.c:768)
            need_pre_release = true;
            if (tap_timer) {
                tap_state = kTapTapped;
                tap_up_ms = milli_timestamp;
                tap_up_x = tap_down_x;
                tap_up_y = tap_down_y;
                hold_virtual_button = true;
                synth_finger_count = 1;
                tap_timer->setTimeoutMS(kTappedHoldMs);
            } else {
                tap_state = kTapIdle;
            }
        } else if (tap_state == kTapDragging || tap_state == kTapDragging2) {
            bool near_edge = (last_drag_x <= kEdgeMarginX) || (last_drag_x >= (MT2_MAX_X - kEdgeMarginX)) ||
                             (last_drag_y <= kEdgeMarginY) || (last_drag_y >= (MT2_MAX_Y - kEdgeMarginY));
            if (near_edge && tap_timer) {
                tap_state = kTapDraggingWait;
                tap_up_x = last_drag_x;
                tap_up_y = last_drag_y;
                hold_virtual_button = true;
                synth_finger_count = 1;
                tap_timer->setTimeoutMS(kEdgeDragWaitMs);
            } else {
                tap_state = kTapIdle;
            }
        } else if (tap_state == kTapTapped || tap_state == kTapDraggingWait) {
            hold_virtual_button = true;
            synth_finger_count = 1;
        } else if (tap_state == kTapRightClickPending) {
            hold_virtual_button = true;
            synth_finger_count = 2;
        }
    }

    if (need_pre_release) {
        MAGIC_TRACKPAD_INPUT_REPORT saved_hdr = *input_report;
        input_report->Button = 0;
        input_report->TouchActive = 0x2;
        input_report_buffer->setLength(sizeof(MAGIC_TRACKPAD_INPUT_REPORT));
        sendReport();
        *input_report = saved_hdr;
    }

    int report_contact_count = multitouch_event.contact_count;

    if (tap_state == kTapSecondPending && active_finger_count == 1 && first_active_idx >= 0) {
        input_report->Button = 1;
        input_report->FINGERS[first_active_idx].Pressure = 120;
        input_report->FINGERS[first_active_idx].X = (SInt16)(tap_down_x - (MT2_MAX_X / 2));
        input_report->FINGERS[first_active_idx].Y = (SInt16)(tap_down_y - (MT2_MAX_Y / 2)) * -1;
    } else if ((tap_state == kTapDragging || tap_state == kTapDragging2) && first_active_idx >= 0) {
        input_report->Button = 1;
        input_report->FINGERS[first_active_idx].Pressure = 120;
    } else if (hold_virtual_button && synth_finger_count > 0) {
        // 0x281 Force Touch driver requires active finger contact(s) alongside Button=1
        input_report->Button = 1;
        report_contact_count = synth_finger_count;
        for (int f = 0; f < synth_finger_count; f++) {
            MAGIC_TRACKPAD_INPUT_REPORT_FINGER& fd = input_report->FINGERS[f];
            SInt32 fx = tap_up_x + (f == 0 ? 0 : 400);
            if (fx > MT2_MAX_X - 100) fx = tap_up_x - 400;
            fd.X = (SInt16)(fx - (MT2_MAX_X / 2));
            fd.Y = (SInt16)(tap_up_y - (MT2_MAX_Y / 2)) * -1;
            fd.Finger = (MT2FingerType)(kMT2FingerTypeIndexFinger + f);
            fd.State = kTouchStateActive;
            fd.Touch_Major = 20;
            fd.Touch_Minor = 20;
            fd.Size = 10;
            fd.Pressure = 120;
            fd.Angle = 0x4;
            fd.Identifier = f + 1;
        }
    }

    bool input_active = fingers_active || physical_button || hold_virtual_button;

    if (input_active)
        input_report->TouchActive = 0x3;
    else
        input_report->TouchActive = 0x2;

    vm_size_t total_report_len = sizeof(MAGIC_TRACKPAD_INPUT_REPORT) +
        sizeof(MAGIC_TRACKPAD_INPUT_REPORT_FINGER) * report_contact_count;

    if (!is_error_input_active) {
        input_report_buffer->setLength(total_report_len);
        sendReport();
    }
    
    if (!input_active) {
        memset(touch_active, false, sizeof(touch_active));

        input_report->FINGERS[0].Size = 0x0;
        input_report->FINGERS[0].Pressure = 0x0;
        input_report->FINGERS[0].Touch_Major = 0x0;
        input_report->FINGERS[0].Touch_Minor = 0x0;

        milli_timestamp += 10;

        input_report->timestamp_buffer[0] = (milli_timestamp << 0x3) | 0x4;
        input_report->timestamp_buffer[1] = (milli_timestamp >> 0x5) & 0xFF;
        input_report->timestamp_buffer[2] = (milli_timestamp >> 0xd) & 0xFF;
        input_report_buffer->setLength(total_report_len);
        sendReport();

        input_report->FINGERS[0].Finger = kMT2FingerTypeUndefined;
        input_report->FINGERS[0].State = kTouchStateInactive;
        input_report_buffer->setLength(total_report_len);
        sendReport();

        milli_timestamp += 10;

        input_report->timestamp_buffer[0] = (milli_timestamp << 0x3) | 0x4;
        input_report->timestamp_buffer[1] = (milli_timestamp >> 0x5) & 0xFF;
        input_report->timestamp_buffer[2] = (milli_timestamp >> 0xd) & 0xFF;
        input_report_buffer->setLength(sizeof(MAGIC_TRACKPAD_INPUT_REPORT));
        sendReport();
    }

    memset(input_report, 0, total_report_len);
}

void VoodooInputSimulatorDevice::onTapTimerTimeout(IOTimerEventSource* sender) {
    if (tap_state == kTapTapped || tap_state == kTapDraggingWait || tap_state == kTapRightClickPending) {
        tap_state = kTapIdle;
        sendButtonReleaseReport();
    }
}

void VoodooInputSimulatorDevice::sendButtonReleaseReport() {
    if (!ready_for_reports || !input_report || !input_report_buffer)
        return;

    memset(touch_active, false, sizeof(touch_active));
    memset(input_report, 0, sizeof(MAGIC_TRACKPAD_INPUT_REPORT) + sizeof(MAGIC_TRACKPAD_INPUT_REPORT_FINGER));

    AbsoluteTime now_abs;
    clock_get_uptime(&now_abs);
    SUB_ABSOLUTETIME(&now_abs, &start_timestamp);
    UInt64 milli_timestamp;
    absolutetime_to_nanoseconds(now_abs, &milli_timestamp);
    milli_timestamp /= 1000000;

    input_report->ReportID = 0x02;
    input_report->Button = 0;
    input_report->TouchActive = 0x2;
    input_report->multitouch_report_id = 0x31;
    input_report->timestamp_buffer[0] = (milli_timestamp << 0x3) | 0x4;
    input_report->timestamp_buffer[1] = (milli_timestamp >> 0x5) & 0xFF;
    input_report->timestamp_buffer[2] = (milli_timestamp >> 0xd) & 0xFF;

    // Frame 1: Button=0 + finger stop
    MAGIC_TRACKPAD_INPUT_REPORT_FINGER& fd = input_report->FINGERS[0];
    fd.X = (SInt16)(tap_up_x - (MT2_MAX_X / 2));
    fd.Y = (SInt16)(tap_up_y - (MT2_MAX_Y / 2)) * -1;
    fd.Finger = kMT2FingerTypeIndexFinger;
    fd.State = kTouchStateStop;
    fd.Identifier = 1;
    input_report_buffer->setLength(sizeof(MAGIC_TRACKPAD_INPUT_REPORT) + sizeof(MAGIC_TRACKPAD_INPUT_REPORT_FINGER));
    sendReport();

    // Frame 2: Inactive + empty lift-off
    milli_timestamp += 10;
    input_report->timestamp_buffer[0] = (milli_timestamp << 0x3) | 0x4;
    input_report->timestamp_buffer[1] = (milli_timestamp >> 0x5) & 0xFF;
    input_report->timestamp_buffer[2] = (milli_timestamp >> 0xd) & 0xFF;
    input_report_buffer->setLength(sizeof(MAGIC_TRACKPAD_INPUT_REPORT));
    sendReport();

    memset(input_report, 0, sizeof(MAGIC_TRACKPAD_INPUT_REPORT) + sizeof(MAGIC_TRACKPAD_INPUT_REPORT_FINGER));
}

bool VoodooInputSimulatorDevice::start(IOService* provider) {
    if (!super::start(provider))
        return false;
    ready_for_reports = false;

    input_report_buffer = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task, 0,
        sizeof(MAGIC_TRACKPAD_INPUT_REPORT) + sizeof(MAGIC_TRACKPAD_INPUT_REPORT_FINGER) * VOODOO_INPUT_MAX_TRANSDUCERS);
    if (!input_report_buffer) {
        IOLog("%s Could not allocate IOBufferMemoryDescriptor\n", getName());
        return false;
    }
    input_report = (MAGIC_TRACKPAD_INPUT_REPORT *) input_report_buffer->getBytesNoCopy();

    clock_get_uptime(&start_timestamp);
    
    engine = OSDynamicCast(VoodooInput, provider);
    
    if (!engine) {
        releaseResources();
        return false;
    }

    work_loop = this->getWorkLoop();
    if (!work_loop) {
        IOLog("%s Could not get a IOWorkLoop instance\n", getName());
        releaseResources();
        return false;
    }
    
    work_loop->retain();
    
    command_gate = IOCommandGate::commandGate(this);
    if (!command_gate || (work_loop->addEventSource(command_gate) != kIOReturnSuccess)) {
        IOLog("%s Could not open command gate\n", getName());
        releaseResources();
        return false;
    }

    tap_timer = IOTimerEventSource::timerEventSource(this, OSMemberFunctionCast(IOTimerEventSource::Action, this, &VoodooInputSimulatorDevice::onTapTimerTimeout));
    if (!tap_timer || (work_loop->addEventSource(tap_timer) != kIOReturnSuccess)) {
        IOLog("%s Could not create tap timer\n", getName());
        releaseResources();
        return false;
    }

    PMinit();
    provider->joinPMtree(this);
    registerPowerDriver(this, PMPowerStates, kIOPMNumberPowerStates);
        
    new_get_report_buffer = nullptr;
    
    ready_for_reports = true;
    
    return true;
}

void VoodooInputSimulatorDevice::stop(IOService* provider) {
    releaseResources();
    
    PMstop();
    
    super::stop(provider);
}

IOReturn VoodooInputSimulatorDevice::setPowerState(unsigned long whichState, IOService* whatDevice) {
    if (whatDevice != this)
        return kIOReturnInvalid;
    if (whichState == 0) {
        // ready_for_reports = false;
    } else {
        // ready_for_reports = true;
    }
    return kIOPMAckImplied;
}

void VoodooInputSimulatorDevice::releaseResources() {
    if (tap_timer) {
        tap_timer->cancelTimeout();
        if (work_loop) {
            work_loop->removeEventSource(tap_timer);
        }
        OSSafeReleaseNULL(tap_timer);
    }
    if (command_gate) {
        work_loop->removeEventSource(command_gate);
        OSSafeReleaseNULL(command_gate);
    }
    input_report = nullptr;

    OSSafeReleaseNULL(work_loop);

    OSSafeReleaseNULL(new_get_report_buffer);

    OSSafeReleaseNULL(input_report_buffer);
}

IOReturn VoodooInputSimulatorDevice::setReport(IOMemoryDescriptor* report, IOHIDReportType reportType, IOOptionBits options) {
    UInt32 report_id = options & 0xFF;
    
    if (report_id == 0x1) {
        char* raw_buffer = (char*)IOMalloc(report->getLength());
        
        report->prepare();
        
        report->readBytes(0, raw_buffer, report->getLength());
        
        report->complete();
        
        if (new_get_report_buffer) {
            new_get_report_buffer->release();
        }
        
        new_get_report_buffer = OSData::withCapacity(1);
        
        if (!new_get_report_buffer) {
            return kIOReturnNoResources;
        }
        
        UInt8 value = raw_buffer[1];
        
        if (value == 0xDB) {
            unsigned char buffer[] = {0x1, 0xDB, 0x00, 0x49, 0x00};
            new_get_report_buffer->appendBytes(buffer, sizeof(buffer));
        }
        
        if (value == 0xD1) {
            unsigned char buffer[] = {0x1, 0xD1, 0x00, 0x01, 0x00};
            new_get_report_buffer->appendBytes(buffer, sizeof(buffer));
        }
        
        if (value == 0xD3) {
            unsigned char buffer[] = {0x1, 0xD3, 0x00, 0x0C, 0x00};
            new_get_report_buffer->appendBytes(buffer, sizeof(buffer));
        }
        
        if (value == 0xD0) {
            unsigned char buffer[] = {0x1, 0xD0, 0x00, 0x0F, 0x00};
            new_get_report_buffer->appendBytes(buffer, sizeof(buffer));
        }
        
        if (value == 0xA1) {
            unsigned char buffer[] = {0x1, 0xA1, 0x00, 0x06, 0x00};
            new_get_report_buffer->appendBytes(buffer, sizeof(buffer));
        }
        
        if (value == 0xD9) {
            //Sensor Surface Width = 0x3cf0 (0xf0, 0x3c) = 15.600 cm
            //Sensor Surface Height = 0x2b20 (0x20, 0x2b) = 11.040 cm

            // It's already in 0.01 mm units
            uint32_t rawWidth = engine->getPhysicalMaxX();
            uint32_t rawHeight = engine->getPhysicalMaxY();

            uint8_t rawWidthLower = rawWidth & 0xff;
            uint8_t rawWidthHigher = (rawWidth >> 8) & 0xff;

            uint8_t rawHeightLower = rawHeight & 0xff;
            uint8_t rawHeightHigher = (rawHeight >> 8) & 0xff;

            unsigned char buffer[] = {0xD9, rawWidthLower, rawWidthHigher, 0x00, 0x00, rawHeightLower, rawHeightHigher, 0x00, 0x00, 0x44, 0xE3, 0x52, 0xFF, 0xBD, 0x1E, 0xE4, 0x26}; //Sensor Surface Description
            new_get_report_buffer->appendBytes(buffer, sizeof(buffer));
        }
        
        if (value == 0x7F) {
            unsigned char buffer[] = {0x1, 0x7F, 0x00, 0x04, 0x00};
            new_get_report_buffer->appendBytes(buffer, sizeof(buffer));
        }
        
        if (value == 0xC8) {
            unsigned char buffer[] = {0x1, 0xC8, 0x00, 0x01, 0x00};
            new_get_report_buffer->appendBytes(buffer, sizeof(buffer));
        }
        
        IOFree(raw_buffer, report->getLength());
    }
    
    return kIOReturnSuccess;
}

IOReturn VoodooInputSimulatorDevice::getReport(IOMemoryDescriptor* report, IOHIDReportType reportType, IOOptionBits options) {
    UInt32 report_id = options & 0xFF;
    Boolean getReportedAllocated = true;
    
    OSData* get_buffer = OSData::withCapacity(1);

    if (!get_buffer || (report_id == 0x1 && !new_get_report_buffer)) {
       OSSafeReleaseNULL(get_buffer);
       return kIOReturnNoResources;
    }
    
    if (report_id == 0x0) {
        unsigned char buffer[] = {0x0, 0x01};
        get_buffer->appendBytes(buffer, sizeof(buffer));
    }
    
    if (report_id == 0x1) {
        get_buffer->release();
        get_buffer = new_get_report_buffer;
        getReportedAllocated = false;
    }
    
    if (report_id == 0xD1) {
        unsigned char buffer[] = {0xD1, 0x81};
        // Family ID = 0x81
        get_buffer->appendBytes(buffer, sizeof(buffer));
    }
    
    if (report_id == 0xD3) {
        unsigned char buffer[] = {0xD3, 0x01, 0x16, 0x1E, 0x03, 0x95, 0x00, 0x14, 0x1E, 0x62, 0x05, 0x00, 0x00};
        // Sensor Rows = 0x16
        // Sensor Columns = 0x1e
        get_buffer->appendBytes(buffer, sizeof(buffer));
    }
    
    if (report_id == 0xD0) {
        unsigned char buffer[] = {0xD0, 0x02, 0x01, 0x00, 0x14, 0x01, 0x00, 0x1E, 0x00, 0x02, 0x14, 0x02, 0x01, 0x0E, 0x02, 0x00}; // Sensor Region Description
        get_buffer->appendBytes(buffer, sizeof(buffer));
    }
    
    if (report_id == 0xA1) {
        unsigned char buffer[] = {0xA1, 0x00, 0x00, 0x05, 0x00, 0xFC, 0x01}; // Sensor Region Param
        get_buffer->appendBytes(buffer, sizeof(buffer));
    }
    
    if (report_id == 0xD9) {
        // Sensor Surface Width = 0x3cf0 (0xf0, 0x3c) = 15.600 cm
        // Sensor Surface Height = 0x2b20 (0x20, 0x2b) = 11.040 cm
        
        uint32_t rawWidth = engine->getPhysicalMaxX();
        uint32_t rawHeight = engine->getPhysicalMaxY();
        
        uint8_t rawWidthLower = rawWidth & 0xff;
        uint8_t rawWidthHigher = (rawWidth >> 8) & 0xff;
        
        uint8_t rawHeightLower = rawHeight & 0xff;
        uint8_t rawHeightHigher = (rawHeight >> 8) & 0xff;
        
        unsigned char buffer[] = {0xD9, rawWidthLower, rawWidthHigher, 0x00, 0x00, rawHeightLower, rawHeightHigher, 0x00, 0x00, 0x44, 0xE3, 0x52, 0xFF, 0xBD, 0x1E, 0xE4, 0x26}; // Sensor Surface Description
        get_buffer->appendBytes(buffer, sizeof(buffer));
    }
    
    if (report_id == 0x7F) {
        unsigned char buffer[] = {0x7F, 0x00, 0x00, 0x00, 0x00};
        get_buffer->appendBytes(buffer, sizeof(buffer));
    }
    
    if (report_id == 0xC8) {
        unsigned char buffer[] = {0xC8, 0x08};
        get_buffer->appendBytes(buffer, sizeof(buffer));
    }
    
    if (report_id == 0x2) {
        unsigned char buffer[] = {0x02, 0x01};
        get_buffer->appendBytes(buffer, sizeof(buffer));
    }
    
    if (report_id == 0xDB) {
        uint32_t rawWidth = engine->getPhysicalMaxX();
        uint32_t rawHeight = engine->getPhysicalMaxY();
        
        uint8_t rawWidthLower = rawWidth & 0xff;
        uint8_t rawWidthHigher = (rawWidth >> 8) & 0xff;
        
        uint8_t rawHeightLower = rawHeight & 0xff;
        uint8_t rawHeightHigher = (rawHeight >> 8) & 0xff;
        
        unsigned char buffer[] = {0xDB, 0x01, 0x02, 0x00,
            /* Start 0xD1 */ 0xD1, 0x81, /* End 0xD1 */
            0x0D, 0x00,
            /* Start 0xD3 */ 0xD3, 0x01, 0x16, 0x1E, 0x03, 0x95, 0x00, 0x14, 0x1E, 0x62, 0x05, 0x00, 0x00, /* End 0xD3 */
            0x10, 0x00,
            /* Start 0xD0 */ 0xD0, 0x02, 0x01, 0x00, 0x14, 0x01, 0x00, 0x1E, 0x00, 0x02, 0x14, 0x02, 0x01, 0x0E, 0x02, 0x00, /* End 0xD0 */
            0x07, 0x00,
            /* Start 0xA1 */ 0xA1, 0x00, 0x00, 0x05, 0x00, 0xFC, 0x01, /* End 0xA1 */
            0x11, 0x00,
            /* Start 0xD9 */ 0xD9, rawWidthLower, rawWidthHigher, 0x00, 0x00, rawHeightLower, rawHeightHigher, 0x00, 0x00, 0x44, 0xE3, 0x52, 0xFF, 0xBD, 0x1E, 0xE4, 0x26,
            /* Start 0x7F */ 0x7F, 0x00, 0x00, 0x00, 0x00 /*End 0x7F */};
        get_buffer->appendBytes(buffer, sizeof(buffer));
    }
    
    report->writeBytes(0, get_buffer->getBytesNoCopy(), get_buffer->getLength());
    
   if (getReportedAllocated) {
        get_buffer->release();
   }
    
    return kIOReturnSuccess;
}

IOReturn VoodooInputSimulatorDevice::newReportDescriptor(IOMemoryDescriptor** descriptor) const {
    IOBufferMemoryDescriptor* report_descriptor_buffer = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task, 0, sizeof(report_descriptor));
    
    if (!report_descriptor_buffer) {
        IOLog("%s Could not allocate buffer for report descriptor\n", getName());
        return kIOReturnNoResources;
    }
    
    report_descriptor_buffer->writeBytes(0, report_descriptor, sizeof(report_descriptor));
    *descriptor = report_descriptor_buffer;
    
    return kIOReturnSuccess;
}

OSString* VoodooInputSimulatorDevice::newManufacturerString() const {
    return OSString::withCString("Apple Inc.");
}

OSNumber* VoodooInputSimulatorDevice::newPrimaryUsageNumber() const {
    return OSNumber::withNumber(kHIDUsage_GD_Mouse, 32);
}

OSNumber* VoodooInputSimulatorDevice::newPrimaryUsagePageNumber() const {
    return OSNumber::withNumber(kHIDPage_GenericDesktop, 32);
}

OSNumber* VoodooInputSimulatorDevice::newProductIDNumber() const {
    return OSNumber::withNumber(VoodooInputGetProductId(), 32);
}

OSString* VoodooInputSimulatorDevice::newProductString() const {
    return OSString::withCString("Magic Trackpad 2");
}

OSString* VoodooInputSimulatorDevice::newSerialNumberString() const {
    return OSString::withCString("Voodoo Magic Trackpad 2 Simulator");
}

OSString* VoodooInputSimulatorDevice::newTransportString() const {
    return OSString::withCString("I2C");
}

OSNumber* VoodooInputSimulatorDevice::newVendorIDNumber() const {
    return OSNumber::withNumber(kVoodooInputVendorApple, 16);
}

OSNumber* VoodooInputSimulatorDevice::newLocationIDNumber() const {
    return OSNumber::withNumber(0x14400000, 32);
}

OSNumber* VoodooInputSimulatorDevice::newVersionNumber() const {
    return OSNumber::withNumber(0x804, 32);
}
