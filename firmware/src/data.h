#pragma once
#include <Arduino.h>

enum UsageMode {
    MODE_UNKNOWN = 0,
    MODE_SUBSCRIPTION,   // Claude Code subscription: 5h + 7d quota windows
    MODE_API,            // API billing: token spend, $ spend, burn rate
};

struct UsageData {
    UsageMode mode;

    // ---- Subscription mode ----
    float session_pct;          // 5-hour utilization (0..100)
    int   session_reset_mins;   // mins until 5h reset
    float weekly_pct;           // 7-day utilization (0..100)
    int   weekly_reset_mins;    // mins until 7d reset

    // ---- API billing mode ----
    uint64_t tokens_used;
    uint64_t tokens_quota;      // 0 = no quota set
    int      dollars_spent_cents;   // current-cycle spend, in cents
    int      dollars_budget_cents;  // 0 = no budget
    int      burn_per_min;          // tokens/min, instantaneous
    int      burn_pct_change;       // % delta from avg, signed (+12 = up)
    int      api_reset_mins;        // mins until billing cycle resets
    char     period_label[16];      // e.g. "May 2026"

    // ---- Animation screen ----
    uint64_t ctx_used;          // tokens in current Claude context
    uint64_t ctx_max;           // context window limit (0 = hide)
    bool     session_active;    // Claude Code mid-turn → working/anim view
    uint32_t task_tokens;       // output tokens for the current task
    uint32_t task_seconds;      // elapsed seconds since the current task began
    char     model_label[24];   // e.g. "Opus 4.7 1M" (empty if unknown)
    char     phase[12];         // "thinking" / "lightbulb" / "working"

    char status[16];
    bool ok;
    bool valid;
};
