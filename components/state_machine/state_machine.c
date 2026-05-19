#include "state_machine.h"

#include <stdlib.h>
#include <string.h>

struct state_machine {
    const config_t *cfg;
    sm_callbacks_t cb;

    uint8_t current_bank;
    uint8_t target_bank;
    uint8_t current_slot;

    // Per-slot alt cache (spec 2.5):
    //   ALT_A: only alt_active[current_slot] is meaningful — switching slots
    //          clears the whole array (cheaper than tracking which entry is the
    //          "live" one).
    //   ALT_B: every entry is meaningful within the current bank, preserved
    //          across slot selections, cleared on bank commit.
    bool alt_active[PROGRAMS_PER_BANK];

    bool tuner_on;
    uint32_t browse_started_ms;
};

static const program_slot_t *slot_at(const state_machine_t *sm, uint8_t bank, uint8_t slot)
{
    return &sm->cfg->banks[bank].programs[slot];
}

static void emit_message_list(state_machine_t *sm, const midi_message_list_t *list)
{
    for (uint8_t i = 0; i < list->count; i++) {
        sm->cb.send_midi(&list->msgs[i], sm->cb.ctx);
    }
}

static void emit_slot_messages(state_machine_t *sm, uint8_t slot_idx, bool alt)
{
    const program_slot_t *slot = slot_at(sm, sm->current_bank, slot_idx);
    const program_payload_t *p = alt ? &slot->alternative : &slot->primary;
    // Empty slot (spec 2.4): activates the slot but emits no MIDI.
    if (p->messages.count == 0) {
        return;
    }
    emit_message_list(sm, &p->messages);
}

static void notify_state_changed(state_machine_t *sm)
{
    if (sm->cb.state_changed) {
        sm->cb.state_changed(sm->cb.ctx);
    }
}

static void clear_alt_cache(state_machine_t *sm)
{
    memset(sm->alt_active, 0, sizeof(sm->alt_active));
}

state_machine_t *state_machine_create(const config_t *cfg, const sm_callbacks_t *cb)
{
    if (!cfg || !cb || !cb->send_midi || !cb->now_ms) {
        return NULL;
    }
    state_machine_t *sm = calloc(1, sizeof(*sm));
    if (!sm) {
        return NULL;
    }
    sm->cfg = cfg;
    sm->cb = *cb;
    // Initial state per spec 2.3: current = target = 0. boot_resume restoration
    // happens elsewhere (ConfigStore) and overrides current via a future setter.
    sm->current_bank = 0;
    sm->target_bank = 0;
    sm->current_slot = 0;
    sm->tuner_on = false;
    sm->browse_started_ms = 0;
    return sm;
}

void state_machine_destroy(state_machine_t *sm)
{
    free(sm);
}

static void bank_step(state_machine_t *sm, int8_t delta)
{
    if (sm->cfg->bank_count == 0) {
        return;
    }
    uint8_t was_browsing = (sm->target_bank != sm->current_bank);
    uint8_t n = sm->cfg->bank_count;
    sm->target_bank = (uint8_t)((sm->target_bank + (int)delta + n) % n);

    if (sm->target_bank != sm->current_bank) {
        // Entering or staying in browse mode — reset the timeout window.
        sm->browse_started_ms = sm->cb.now_ms(sm->cb.ctx);
    } else if (was_browsing) {
        // Implicit cancel (spec 2.3.4): navigated back to current.
        sm->browse_started_ms = 0;
    }
    notify_state_changed(sm);
}

static void handle_program_press(state_machine_t *sm, uint8_t slot_idx)
{
    if (slot_idx >= PROGRAMS_PER_BANK) {
        return;
    }
    if (sm->target_bank != sm->current_bank) {
        // BROWSE COMMIT (spec 2.3.4): jump banks, activate slot in primary,
        // clear alt cache for the whole new bank.
        sm->current_bank = sm->target_bank;
        sm->current_slot = slot_idx;
        clear_alt_cache(sm);
        sm->browse_started_ms = 0;
        emit_slot_messages(sm, slot_idx, false);
        notify_state_changed(sm);
        return;
    }

    if (slot_idx != sm->current_slot) {
        // Different slot within the same bank (spec 2.4 + 2.5).
        if (sm->cfg->global.alt_toggle_behavior == ALT_TOGGLE_A) {
            // ALT_A: new slot starts in primary, and old slot's toggle state
            // is no longer meaningful.
            clear_alt_cache(sm);
        }
        sm->current_slot = slot_idx;
        const program_slot_t *slot = slot_at(sm, sm->current_bank, slot_idx);
        // If a cached alt state points at a slot that has no alternative,
        // fall back to primary (defensive — config can change while running).
        if (sm->alt_active[slot_idx] && !slot->has_alternative) {
            sm->alt_active[slot_idx] = false;
        }
        emit_slot_messages(sm, slot_idx, sm->alt_active[slot_idx]);
        notify_state_changed(sm);
        return;
    }

    // Same slot pressed again — alt toggle (spec 2.5). No-op if no alternative
    // is configured.
    const program_slot_t *slot = slot_at(sm, sm->current_bank, slot_idx);
    if (!slot->has_alternative) {
        return;
    }
    sm->alt_active[slot_idx] = !sm->alt_active[slot_idx];
    emit_slot_messages(sm, slot_idx, sm->alt_active[slot_idx]);
    notify_state_changed(sm);
}

static void handle_tap_short(state_machine_t *sm)
{
    sm->cb.send_midi(&sm->cfg->global.tap_message, sm->cb.ctx);
}

static void handle_tap_long(state_machine_t *sm)
{
    sm->tuner_on = !sm->tuner_on;
    sm->cb.send_midi(&sm->cfg->global.tuner_message, sm->cb.ctx);
    notify_state_changed(sm);
}

static void handle_footswitch(state_machine_t *sm, sm_footswitch_t fs, sm_press_kind_t press)
{
    if (press == SM_PRESS_SHORT) {
        switch (fs) {
        case SM_FS_BANK_DOWN: bank_step(sm, -1); break;
        case SM_FS_BANK_UP:   bank_step(sm, +1); break;
        case SM_FS_PROG_1:
        case SM_FS_PROG_2:
        case SM_FS_PROG_3:
        case SM_FS_PROG_4:
        case SM_FS_PROG_5:
            handle_program_press(sm, (uint8_t)(fs - SM_FS_PROG_1));
            break;
        case SM_FS_TAP: handle_tap_short(sm); break;
        default: break;
        }
    } else {
        switch (fs) {
        case SM_FS_BANK_DOWN:
            emit_message_list(sm, &sm->cfg->global.user_function_a);
            break;
        case SM_FS_BANK_UP:
            emit_message_list(sm, &sm->cfg->global.user_function_b);
            break;
        case SM_FS_TAP: handle_tap_long(sm); break;
        case SM_FS_PROG_1:
        case SM_FS_PROG_2:
        case SM_FS_PROG_3:
        case SM_FS_PROG_4:
        case SM_FS_PROG_5:
            // Long press on program buttons is intentionally undefined (spec 2.2).
            break;
        default: break;
        }
    }
}

static void handle_tick(state_machine_t *sm)
{
    if (sm->target_bank == sm->current_bank) {
        return;  // not in browse mode
    }
    uint32_t timeout = sm->cfg->global.browse_timeout_ms;
    if (timeout == 0) {
        return;  // timeout disabled
    }
    uint32_t now = sm->cb.now_ms(sm->cb.ctx);
    if ((uint32_t)(now - sm->browse_started_ms) >= timeout) {
        // Timeout (spec 2.3.4): drop browse mode silently.
        sm->target_bank = sm->current_bank;
        sm->browse_started_ms = 0;
        notify_state_changed(sm);
    }
}

void state_machine_dispatch(state_machine_t *sm, const sm_event_t *evt)
{
    if (!sm || !evt) {
        return;
    }
    switch (evt->type) {
    case SM_EVT_FOOTSWITCH:
        handle_footswitch(sm, evt->footswitch.fs, evt->footswitch.press);
        break;
    case SM_EVT_TICK:
        handle_tick(sm);
        break;
    case SM_EVT_EXPRESSION:
        // Expression routing (CC# + channel resolution + MIDI emission) is
        // a future step alongside the ExpressionPedal module — placeholder.
        (void)evt->expression.value;
        break;
    }
}

void state_machine_snapshot(const state_machine_t *sm, sm_snapshot_t *out)
{
    if (!sm || !out) {
        return;
    }
    out->current_bank = sm->current_bank;
    out->target_bank = sm->target_bank;
    out->current_slot = sm->current_slot;
    out->in_browse_mode = (sm->target_bank != sm->current_bank);
    out->tuner_on = sm->tuner_on;

    const program_slot_t *cur = slot_at(sm, sm->current_bank, sm->current_slot);
    out->current_slot_empty = (cur->primary.messages.count == 0);
    out->current_slot_alt_active = sm->alt_active[sm->current_slot] && cur->has_alternative;

    for (uint8_t i = 0; i < PROGRAMS_PER_BANK; i++) {
        bool is_current = (i == sm->current_slot);
        if (out->in_browse_mode) {
            if (is_current) {
                out->slot_leds[i] = out->current_slot_alt_active
                    ? SM_SLOT_LED_BROWSE_ACTIVE_ALT
                    : SM_SLOT_LED_BROWSE_ACTIVE_PRIMARY;
            } else {
                out->slot_leds[i] = SM_SLOT_LED_BROWSE_INACTIVE;
            }
        } else {
            if (is_current) {
                out->slot_leds[i] = out->current_slot_alt_active
                    ? SM_SLOT_LED_ACTIVE_ALT
                    : SM_SLOT_LED_ACTIVE_PRIMARY;
            } else {
                out->slot_leds[i] = SM_SLOT_LED_INACTIVE;
            }
        }
    }
}
