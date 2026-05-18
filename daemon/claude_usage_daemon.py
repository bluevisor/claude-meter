#!/usr/bin/env python3
"""Claude Usage Tracker Daemon (BLE) — macOS port of claude-usage-daemon.sh.

Polls Claude usage and writes a JSON payload to the ESP32 "Claude
Controller" peripheral over a custom GATT service. Uses bleak
(CoreBluetooth backend on macOS).

Three modes are supported, picked per-tick:

* "session" (preferred) — pulls real-time data straight from the
  Claude Code session JSONL transcripts under
  ~/.claude/projects/*/<session>.jsonl. This is the same source
  Claude Code's statusline reads, so the meter mirrors what you see
  in the CLI. Updates are pushed within a few seconds of activity by
  watching the JSONL mtime.

* "api" — direct Anthropic API billing through a LiteLLM proxy. The
  daemon reads a config file (path below) describing the LiteLLM admin
  endpoint + key, then hits LiteLLM's user/info endpoint to get the
  tokens spent and the token budget for the current period.

* "subscription" — Claude Code subscription quotas (OAuth credentials
  in the macOS keychain or ~/.claude/.credentials.json) — only used
  as a fallback when no active session is detected.

Resolution per tick:
  1. Active JSONL session within SESSION_FRESH_SECONDS? → session
  2. LiteLLM config present? → api
  3. OAuth token present? → subscription
"""

import asyncio
import getpass
import json
import os
import re
import signal
import subprocess
import sys
import time
from datetime import datetime, timedelta
from pathlib import Path

import httpx
from bleak import BleakClient, BleakScanner
from bleak.exc import BleakError

DEVICE_NAME = "Claude Meter"
# Bleak/CoreBluetooth has cached the old name on macOS for some users; allow
# matching either label so the daemon connects to existing units.
DEVICE_NAME_ALIASES = ("Claude Meter", "Claude Controller")
SERVICE_UUID = "4c41555a-4465-7669-6365-000000000001"
RX_CHAR_UUID = "4c41555a-4465-7669-6365-000000000002"
REQ_CHAR_UUID = "4c41555a-4465-7669-6365-000000000004"

POLL_INTERVAL = 60
# Loop period — controls how fast the daemon notices a JSONL write
# (i.e. how fast the meter flips to working mode after Enter). 300ms
# is "feels instant" without burning host CPU on dir scans.
TICK = 0.3
SCAN_TIMEOUT = 8.0
# When session is active, push at least this often so the on-device
# timer stays anchored — the firmware advances seconds locally
# between pushes so we don't need 1-Hz BLE traffic.
ACTIVE_SYNC_INTERVAL = 15

# Claude Code transcript discovery.
CLAUDE_PROJECTS_DIR = Path.home() / ".claude" / "projects"
# A JSONL counts as "active" (cycle-able from the device) if its mtime
# is within this many seconds. Bumped from 30min → 4h so sessions that
# the user has open in a terminal but hasn't typed in for a while still
# show up in the focus rotation; they only fall off after a long idle.
SESSION_FRESH_SECONDS = 4 * 60 * 60
# Models with a [1m] suffix expose a 1M-token context window; everything
# else is the regular 200k window.
DEFAULT_CONTEXT_MAX = 200_000
LARGE_CONTEXT_MAX   = 1_000_000

# macOS: token lives in Keychain (service "Claude Code-credentials").
# Linux: token lives in ~/.claude/.credentials.json.
KEYCHAIN_SERVICE = "Claude Code-credentials"
CREDENTIALS_PATH = Path.home() / ".claude" / ".credentials.json"
SAVED_ADDR_FILE = Path.home() / ".config" / "claude-usage-monitor" / "ble-address"

# Drop a JSON file at this path to switch the daemon into API-billing mode.
# Shape:
#   {
#     "api_base": "https://litellm.example.com",
#     "api_key":  "sk-...",
#     "user_id":  "optional override; defaults to your shell username"
#   }
LITELLM_CONFIG_PATH = Path.home() / ".config" / "claude-usage-monitor" / "litellm.json"

API_URL = "https://api.anthropic.com/v1/messages"
API_HEADERS_TEMPLATE = {
    "anthropic-version": "2023-06-01",
    "anthropic-beta": "oauth-2025-04-20",
    "Content-Type": "application/json",
    "User-Agent": "claude-code/2.1.5",
}
API_BODY = {
    "model": "claude-haiku-4-5-20251001",
    "max_tokens": 1,
    "messages": [{"role": "user", "content": "hi"}],
}


def log(msg: str) -> None:
    print(f"[{time.strftime('%H:%M:%S')}] {msg}", flush=True)


def _extract_access_token(blob: str) -> str | None:
    """Pull the accessToken out of a credentials blob.

    Claude Code stores credentials as a JSON object; the blob may also be
    nested ({"claudeAiOauth": {"accessToken": "..."}}). Fall back to a
    regex match so unexpected shapes still work, and finally treat the
    blob as a raw token if nothing else matches.
    """
    blob = blob.strip()
    if not blob:
        return None
    try:
        data = json.loads(blob)
    except json.JSONDecodeError:
        data = None
    if isinstance(data, dict):
        # direct: {"accessToken": "..."}
        if isinstance(data.get("accessToken"), str):
            return data["accessToken"]
        # nested: {"claudeAiOauth": {"accessToken": "..."}}
        for v in data.values():
            if isinstance(v, dict) and isinstance(v.get("accessToken"), str):
                return v["accessToken"]
    m = re.search(r'"accessToken"\s*:\s*"([^"]+)"', blob)
    if m:
        return m.group(1)
    # Raw token (no JSON wrapper) — must look plausible (sk-ant-... etc.)
    if re.fullmatch(r"[A-Za-z0-9_\-.~+/=]{20,}", blob):
        return blob
    return None


def _read_token_keychain() -> str | None:
    try:
        out = subprocess.run(
            [
                "security",
                "find-generic-password",
                "-s",
                KEYCHAIN_SERVICE,
                "-a",
                getpass.getuser(),
                "-w",
            ],
            check=True,
            capture_output=True,
            text=True,
            timeout=10,
        )
    except subprocess.CalledProcessError as e:
        log(f"Keychain read failed (rc={e.returncode}): {e.stderr.strip()}")
        return None
    except (FileNotFoundError, subprocess.TimeoutExpired) as e:
        log(f"Keychain access error: {e}")
        return None
    return _extract_access_token(out.stdout)


def _read_token_file() -> str | None:
    try:
        raw = CREDENTIALS_PATH.read_text()
    except OSError as e:
        log(f"Error reading credentials: {e}")
        return None
    return _extract_access_token(raw)


def read_token() -> str | None:
    if sys.platform == "darwin":
        return _read_token_keychain()
    return _read_token_file()


def load_cached_address() -> str | None:
    if not SAVED_ADDR_FILE.exists():
        return None
    addr = SAVED_ADDR_FILE.read_text().strip()
    # Accept both Linux MAC (AA:BB:CC:DD:EE:FF) and macOS CoreBluetooth UUID
    # (E621E1F8-C36C-495A-93FC-0C247A3E6E5F).
    if re.fullmatch(r"(?:[0-9A-Fa-f]{2}:){5}[0-9A-Fa-f]{2}", addr) or re.fullmatch(
        r"[0-9A-Fa-f]{8}-(?:[0-9A-Fa-f]{4}-){3}[0-9A-Fa-f]{12}", addr
    ):
        return addr
    log("Cached address malformed, discarding")
    SAVED_ADDR_FILE.unlink(missing_ok=True)
    return None


def save_address(addr: str) -> None:
    SAVED_ADDR_FILE.parent.mkdir(parents=True, exist_ok=True)
    SAVED_ADDR_FILE.write_text(addr)


async def scan_for_device() -> str | None:
    log(f"Scanning for '{DEVICE_NAME}' ({SCAN_TIMEOUT}s)...")
    devices = await BleakScanner.discover(timeout=SCAN_TIMEOUT)
    for d in devices:
        if d.name in DEVICE_NAME_ALIASES:
            log(f"Found: {d.address} ({d.name})")
            return d.address
    return None


def load_litellm_config() -> dict | None:
    """Return the LiteLLM admin config dict, or None if not configured."""
    if not LITELLM_CONFIG_PATH.exists():
        return None
    try:
        cfg = json.loads(LITELLM_CONFIG_PATH.read_text())
    except (OSError, json.JSONDecodeError) as e:
        log(f"LiteLLM config unreadable ({LITELLM_CONFIG_PATH}): {e}")
        return None
    if not isinstance(cfg, dict) or "api_base" not in cfg or "api_key" not in cfg:
        log(f"LiteLLM config missing api_base/api_key: {LITELLM_CONFIG_PATH}")
        return None
    return cfg


async def poll_subscription(token: str) -> dict | None:
    """Hit Anthropic with the OAuth token; read unified 5h/7d rate-limit headers."""
    headers = dict(API_HEADERS_TEMPLATE)
    headers["Authorization"] = f"Bearer {token}"
    try:
        async with httpx.AsyncClient(timeout=20.0) as http:
            resp = await http.post(API_URL, headers=headers, json=API_BODY)
    except httpx.HTTPError as e:
        log(f"API call failed: {e}")
        # Emit a stub payload so the firmware can surface "api_fail"
        # under the title instead of just freezing on the last good
        # numbers. ok=False signals everything else is unreliable.
        return {"mode": "subscription", "s": 0, "sr": -1, "w": 0, "wr": -1,
                "st": "api_fail", "ok": False}

    def hdr(name: str, default: str = "0") -> str:
        return resp.headers.get(name, default)

    now = time.time()

    def reset_minutes(reset_ts: str) -> int:
        try:
            r = float(reset_ts)
        except ValueError:
            return 0
        mins = (r - now) / 60.0
        return int(round(mins)) if mins > 0 else 0

    def pct(util: str) -> int:
        try:
            return int(round(float(util) * 100))
        except ValueError:
            return 0

    payload = {
        "mode": "subscription",
        "s":  pct(hdr("anthropic-ratelimit-unified-5h-utilization")),
        "sr": reset_minutes(hdr("anthropic-ratelimit-unified-5h-reset")),
        "w":  pct(hdr("anthropic-ratelimit-unified-7d-utilization")),
        "wr": reset_minutes(hdr("anthropic-ratelimit-unified-7d-reset")),
        "st": hdr("anthropic-ratelimit-unified-5h-status", "unknown"),
        "ok": True,
    }
    return payload


# Rolling spend history so we can project burn rate. (spend_dollars, ts).
_api_history: list[tuple[float, float]] = []


def _burn_rate(spend_now: float, now_ts: float) -> tuple[int, int]:
    """Return (dollars_per_day × 100 cents-instant, percent_change_vs_average).

    Tracks the last ~30 min of cumulative spend, projects the recent
    delta out to a 24 h rate. Reported in *cents/day* so the firmware
    can keep using its int field without losing precision.
    """
    _api_history.append((spend_now, now_ts))
    cutoff = now_ts - 30 * 60
    while len(_api_history) > 1 and _api_history[0][1] < cutoff:
        _api_history.pop(0)
    if len(_api_history) < 2:
        return 0, 0
    SEC_PER_DAY = 86400.0
    # Instantaneous burn — last poll interval projected to a day.
    prev_spend, prev_ts = _api_history[-2]
    dt = max(now_ts - prev_ts, 1.0)
    instant_dpd_cents = int(round((spend_now - prev_spend) * SEC_PER_DAY * 100.0 / dt))
    # Average across the window.
    first_spend, first_ts = _api_history[0]
    win = max(now_ts - first_ts, 60.0)
    avg_dpd_cents = int(round((spend_now - first_spend) * SEC_PER_DAY * 100.0 / win))
    pct = 0
    if avg_dpd_cents > 0:
        pct = int(round((instant_dpd_cents - avg_dpd_cents) * 100.0 / avg_dpd_cents))
    return max(instant_dpd_cents, 0), pct


def _next_month_mins(now: datetime) -> int:
    """Mins until first-of-next-month at 00:00 (billing cycle boundary)."""
    if now.month == 12:
        nxt = datetime(now.year + 1, 1, 1)
    else:
        nxt = datetime(now.year, now.month + 1, 1)
    return int((nxt - now).total_seconds() // 60)


async def poll_api_billing(cfg: dict) -> dict | None:
    """Hit LiteLLM admin API for tokens used + quota for the current period."""
    base = cfg["api_base"].rstrip("/")
    headers = {"Authorization": f"Bearer {cfg['api_key']}"}
    user_id = cfg.get("user_id") or os.environ.get("USER") or getpass.getuser()

    try:
        async with httpx.AsyncClient(timeout=15.0) as http:
            resp = await http.get(f"{base}/user/info",
                                  headers=headers,
                                  params={"user_id": user_id})
            resp.raise_for_status()
            info = resp.json()
    except (httpx.HTTPError, ValueError) as e:
        log(f"LiteLLM call failed: {e}")
        return None

    record = info.get("user_info") if isinstance(info, dict) else None
    if not isinstance(record, dict):
        record = info if isinstance(info, dict) else {}

    # LiteLLM's user-level spend is a lifetime total (carries over from
    # deleted keys), and `max_budget` is often null at the user level.
    # When per-key budgets are set, those are the *current-period*
    # numbers and what the device should display. Fall back to the
    # user-level fields only when no key has a configured budget.
    keys = info.get("keys", []) if isinstance(info, dict) else []
    def _fnum(d: dict, k: str) -> float:
        v = d.get(k) if isinstance(d, dict) else None
        try:
            return float(v) if v is not None else 0.0
        except (TypeError, ValueError):
            return 0.0
    key_budget_sum = sum(_fnum(k, "max_budget") for k in keys)
    if key_budget_sum > 0:
        spend_dollars  = sum(_fnum(k, "spend")      for k in keys)
        budget_dollars = key_budget_sum
    else:
        spend_dollars  = _fnum(record, "spend")
        budget_dollars = _fnum(record, "max_budget")

    dpt = float(cfg.get("dollars_per_token", 1e-6))
    tokens_used  = int(record.get("total_tokens", record.get("tokens_used", 0)) or 0)
    tokens_quota = int(record.get("token_budget", record.get("max_tokens",  0)) or 0)
    if not tokens_used and spend_dollars and dpt > 0:
        tokens_used = int(spend_dollars / dpt)
    if not tokens_quota and budget_dollars and dpt > 0:
        tokens_quota = int(budget_dollars / dpt)

    # bm is now dollars/day in cents (renamed semantics, same field on
    # the wire). Firmware label should read "$X.XX/day", not "X k/min".
    burn_per_min, burn_pct = _burn_rate(spend_dollars, time.time())

    now = datetime.now()
    # If LiteLLM exposes a budget_duration like "30d" or "7d" on the
    # key, use that as the period label so the device shows e.g.
    # "$37 / $200 (30d)" instead of the calendar-month default.
    duration = ""
    for k in keys:
        if isinstance(k, dict) and k.get("budget_duration"):
            duration = str(k["budget_duration"])
            break
    period_label = cfg.get("period_label") or duration or now.strftime("%b %Y")
    api_reset_mins = _next_month_mins(now)

    payload: dict = {
        "mode": "api",
        "tu":   tokens_used,
        "tq":   tokens_quota,
        "ds":   int(round(spend_dollars  * 100)),
        "db":   int(round(budget_dollars * 100)),
        "bm":   burn_per_min,
        "bc":   burn_pct,
        "ar":   api_reset_mins,
        "pl":   period_label,
        "st":   "ok",
        "ok":   True,
    }
    # Optional context-window hint for the animation screen.
    if "context_used" in cfg:    payload["cu"] = int(cfg["context_used"])
    if "context_max"  in cfg:    payload["cm"] = int(cfg["context_max"])
    return payload


def find_active_session() -> Path | None:
    """Return the most-recently-touched fresh JSONL, or None."""
    sessions = find_active_sessions()
    return sessions[0] if sessions else None


def find_active_sessions() -> list[Path]:
    """Return all fresh JSONL transcripts, newest first."""
    if not CLAUDE_PROJECTS_DIR.exists():
        return []
    cutoff = time.time() - SESSION_FRESH_SECONDS
    candidates: list[tuple[float, Path]] = []
    for proj in CLAUDE_PROJECTS_DIR.iterdir():
        if not proj.is_dir():
            continue
        for f in proj.glob("*.jsonl"):
            try:
                mtime = f.stat().st_mtime
            except OSError:
                continue
            if mtime >= cutoff:
                candidates.append((mtime, f))
    candidates.sort(key=lambda t: t[0], reverse=True)
    return [p for _, p in candidates]


def _parse_iso_timestamp(ts: str) -> float | None:
    if not ts:
        return None
    # Accept "...Z" by swapping to "+00:00" so fromisoformat accepts it.
    try:
        if ts.endswith("Z"):
            ts = ts[:-1] + "+00:00"
        return datetime.fromisoformat(ts).timestamp()
    except (ValueError, TypeError):
        return None


CLAUDE_SETTINGS_PATH = Path.home() / ".claude" / "settings.json"


def _settings_model() -> str | None:
    """Read the user-selected model from ~/.claude/settings.json.

    The JSONL transcript records the bare model id ("claude-opus-4-7")
    without the variant suffix, but settings.json keeps the configured
    alias (e.g. "opus[1m]") — which is the only place "[1m]" appears
    locally. Read it each call so a /model swap takes effect promptly.
    """
    try:
        return json.loads(CLAUDE_SETTINGS_PATH.read_text()).get("model")
    except (OSError, json.JSONDecodeError, AttributeError):
        return None


def _context_max_for(model: str | None, observed_ctx: int) -> int:
    """Pick a context-window ceiling for the current model.

    Heuristics, in order:
      1. CLAUDE_METER_CONTEXT_MAX env var (manual override).
      2. Either the transcript model or the configured model in
         ~/.claude/settings.json contains "[1m]" — the 1M-context
         variant. settings.json is the authoritative source because
         the JSONL strips that suffix.
      3. Observed context exceeded the standard ceiling — must be 1M.
      4. Default to 200k.
    """
    override = os.environ.get("CLAUDE_METER_CONTEXT_MAX")
    if override:
        try:
            n = int(override)
            if n > 0:
                return n
        except ValueError:
            pass
    settings_model = _settings_model() or ""
    if (model and "[1m]" in model) or "[1m]" in settings_model:
        return LARGE_CONTEXT_MAX
    if observed_ctx > DEFAULT_CONTEXT_MAX:
        return LARGE_CONTEXT_MAX
    return DEFAULT_CONTEXT_MAX


def _is_real_user_message(content: object) -> bool:
    """True if a 'user'-type entry is human input, not a tool_result."""
    if isinstance(content, str):
        return True
    if isinstance(content, list):
        for item in content:
            if isinstance(item, dict) and item.get("type") != "tool_result":
                return True
    return False


# Per-JSONL incremental cache. Lets read_session_usage() skip re-parsing
# the entire transcript on every poll — we just tail the new bytes and
# fold them into the running totals. Keyed by absolute path.
_jsonl_cache: dict[Path, dict] = {}


def _fresh_state() -> dict:
    return {
        "offset": 0,
        "mtime":  0.0,
        "last_usage":   None,
        "last_model":   None,
        "last_ts":      None,
        "first_ts":     None,
        "total_input":  0,
        "total_output": 0,
        "total_cache_read":  0,
        "total_cache_write": 0,
        "task_start_ts":      None,
        "task_output_tokens": 0,
        "prev_block_type": None,
        "last_block_type": None,
        "last_block_ts":   None,
        # Last assistant message's stop_reason — "end_turn" / "tool_use" /
        # "max_tokens" / None. Used to flip "active" to false the moment
        # Claude declares end_turn, without waiting for SESSION_ACTIVE_SECONDS.
        "last_stop_reason": None,
        "last_stop_ts":     None,
        # `baseUrl` captured from any user/assistant record. Non-empty
        # means Claude Code is talking to a proxy (LiteLLM etc) → API
        # mode. Null/empty → talking to api.anthropic.com → subscription.
        "base_url":         None,
        # cwd of the most-recent JSONL record. Lets the daemon match
        # this session to a running `claude` process for env lookup.
        "cwd":              None,
    }


def _fold_line(state: dict, line: str) -> None:
    line = line.strip()
    if not line:
        return
    try:
        obj = json.loads(line)
    except json.JSONDecodeError:
        return
    # Latch baseUrl the first time we see a non-empty one. Claude Code
    # captures whichever endpoint the session was launched against, and
    # it doesn't change mid-session. (Currently 2.1.x leaves this null,
    # so we also capture the session's cwd as a fallback key — the
    # daemon then reads ANTHROPIC_BASE_URL from the running process'
    # env via `ps -E`.)
    if state.get("base_url") in (None, ""):
        bu = obj.get("baseUrl")
        if isinstance(bu, str) and bu.strip():
            state["base_url"] = bu.strip()
    # Track the *latest* cwd, not the first. If the user `cd`'d inside
    # the session, the latest record is what matches the running
    # claude process's current cwd.
    c = obj.get("cwd")
    if isinstance(c, str) and c:
        state["cwd"] = c
    t = obj.get("type")
    ts = _parse_iso_timestamp(obj.get("timestamp", ""))
    if t == "user":
        msg = obj.get("message")
        content = msg.get("content") if isinstance(msg, dict) else None
        if _is_real_user_message(content):
            state["task_start_ts"] = ts
            state["task_output_tokens"] = 0
        return
    if t != "assistant":
        return
    msg = obj.get("message")
    if not isinstance(msg, dict):
        return
    usage = msg.get("usage")
    if not isinstance(usage, dict):
        return
    if ts is not None:
        if state["first_ts"] is None or ts < state["first_ts"]:
            state["first_ts"] = ts
        state["last_ts"] = ts
    state["last_usage"] = usage
    state["last_model"] = msg.get("model") or state["last_model"]
    out_tok = int(usage.get("output_tokens", 0) or 0)
    state["total_input"]  += int(usage.get("input_tokens", 0) or 0)
    state["total_output"] += out_tok
    state["total_cache_write"] += int(usage.get("cache_creation_input_tokens", 0) or 0)
    state["total_cache_read"]  += int(usage.get("cache_read_input_tokens", 0) or 0)
    if state["task_start_ts"] is not None and (ts is None or ts >= state["task_start_ts"]):
        state["task_output_tokens"] += out_tok
    content = msg.get("content")
    block_type: str | None = None
    if isinstance(content, list) and content:
        head = content[0]
        if isinstance(head, dict):
            block_type = head.get("type")
    if block_type:
        state["prev_block_type"] = state["last_block_type"]
        state["last_block_type"] = block_type
        state["last_block_ts"]   = ts
    stop_reason = msg.get("stop_reason")
    if stop_reason is not None:
        state["last_stop_reason"] = stop_reason
        state["last_stop_ts"]     = ts


def read_session_usage(jsonl: Path) -> dict | None:
    """Tail-parse a JSONL transcript and summarize the current session.

    Uses _jsonl_cache so repeated polls only read new bytes since the
    last call. If the file got rewritten or truncated the cache entry
    is rebuilt from scratch.
    """
    try:
        st = jsonl.stat()
    except OSError as e:
        log(f"stat({jsonl}) failed: {e}")
        return None
    state = _jsonl_cache.get(jsonl)
    # Cache miss, file rewritten (older mtime), or truncated (smaller).
    if state is None or st.st_mtime < state["mtime"] or st.st_size < state["offset"]:
        state = _fresh_state()
        _jsonl_cache[jsonl] = state
    try:
        with jsonl.open("r", encoding="utf-8", errors="replace") as f:
            f.seek(state["offset"])
            buf = f.read()
            new_offset = f.tell()
    except OSError as e:
        log(f"Reading session JSONL failed: {e}")
        return None
    # Process complete lines only; stash a trailing partial in the
    # cache by rewinding the offset just before it. JSONL writes are
    # usually line-atomic but a race during a tool result write is
    # cheap to recover from this way.
    if buf:
        if buf.endswith("\n"):
            lines = buf.splitlines()
            consumed = new_offset
        else:
            last_nl = buf.rfind("\n")
            if last_nl < 0:
                lines = []
                consumed = state["offset"]
            else:
                lines = buf[:last_nl].splitlines()
                consumed = state["offset"] + last_nl + 1
        for line in lines:
            _fold_line(state, line)
        state["offset"] = consumed
    state["mtime"] = st.st_mtime

    last_usage = state["last_usage"]
    last_model = state["last_model"]
    if last_usage is None:
        return None
    last_ts            = state["last_ts"]
    first_ts           = state["first_ts"]
    total_input        = state["total_input"]
    total_output       = state["total_output"]
    total_cache_read   = state["total_cache_read"]
    total_cache_write  = state["total_cache_write"]
    task_start_ts      = state["task_start_ts"]
    task_output_tokens = state["task_output_tokens"]
    prev_block_type    = state["prev_block_type"]
    last_block_type    = state["last_block_type"]
    last_block_ts      = state["last_block_ts"]
    last_stop_reason   = state.get("last_stop_reason")
    last_stop_ts       = state.get("last_stop_ts")

    ctx_used = (
        int(last_usage.get("input_tokens", 0) or 0)
        + int(last_usage.get("cache_creation_input_tokens", 0) or 0)
        + int(last_usage.get("cache_read_input_tokens", 0) or 0)
    )
    ctx_max = _context_max_for(last_model, ctx_used)

    now = time.time()
    idle_mins = -1
    if last_ts is not None:
        idle_mins = max(0, int((now - last_ts) / 60))
    age_mins = 0
    if first_ts is not None:
        age_mins = max(0, int((now - first_ts) / 60))

    session_total = total_input + total_output + total_cache_write + total_cache_read
    ctx_pct = 0.0
    if ctx_max > 0:
        ctx_pct = min(100.0, ctx_used * 100.0 / ctx_max)
    # Cumulative tokens as a fraction of the context window — over a long
    # session this naturally exceeds 100, which we cap so the bar pegs.
    sess_pct = 0.0
    if ctx_max > 0:
        sess_pct = min(100.0, session_total * 100.0 / ctx_max)

    task_seconds = 0
    if task_start_ts is not None:
        task_seconds = max(0, int(now - task_start_ts))

    # Phase classifier. `thinking` = the latest assistant block is a
    # thinking trace that just got written; `lightbulb` = the latest
    # block is text/tool_use following a thinking block (idea formed);
    # otherwise default "working".
    phase = "working"
    if last_block_type == "thinking" and last_block_ts and (now - last_block_ts) < 6:
        phase = "thinking"
    elif (prev_block_type == "thinking" and last_block_type in ("text", "tool_use")
          and last_block_ts and (now - last_block_ts) < 1.2):
        phase = "lightbulb"

    return {
        "ctx_used": ctx_used,
        "ctx_max":  ctx_max,
        "ctx_pct":  ctx_pct,
        "session_total": session_total,
        "session_pct":   sess_pct,
        "idle_mins": idle_mins,
        "age_mins":  age_mins,
        "model":     last_model or "",
        "session_id": jsonl.stem,
        "task_tokens":  task_output_tokens,
        "task_seconds": task_seconds,
        "phase":        phase,
        "last_stop_reason": last_stop_reason,
        "last_stop_ts":     last_stop_ts,
        "base_url":         state.get("base_url"),
        "cwd":              state.get("cwd"),
    }


# If the JSONL hasn't been touched in this long, Claude Code is idle.
# 10s is short enough that the on-device "Zzz" idle animation shows up
# within a couple seconds of a turn finishing, while still riding through
# the usual sub-second pauses between tool calls inside a single turn.
SESSION_ACTIVE_SECONDS = 10


def _model_display_label(transcript_model: str, settings_model: str, ctx_max: int) -> str:
    """Build a short, human-readable model name for the usage screen.

    Combines hints from the JSONL `model` (e.g. "claude-opus-4-7") and
    `~/.claude/settings.json` `model` (e.g. "opus[1m]") to produce a
    label like "Opus 4.7 1M" or "Sonnet 4.6".
    """
    base = transcript_model or settings_model or ""
    base = base.replace("[1m]", "").strip()
    name = ""
    # Family — Opus / Sonnet / Haiku, capitalized.
    for fam in ("opus", "sonnet", "haiku"):
        if fam in base.lower():
            name = fam.capitalize()
            break
    # Version like 4-7 → "4.7" (from "claude-opus-4-7").
    m = re.search(r"-([0-9])-([0-9])", base)
    if m and name:
        name += f" {m.group(1)}.{m.group(2)}"
    # 1M tag — settings.json is authoritative since the JSONL strips it.
    if "[1m]" in settings_model or ctx_max >= 1_000_000:
        name += " 1M"
    return name.strip() or "Claude"


# Identity of the currently-focused session, held as a Path so it
# survives find_active_sessions() re-sorting (results are ordered by
# mtime newest-first, which means a stale idx silently lands on a
# different session as soon as anyone else types).
_focused_session_path: Path | None = None


def _resolve_focus_index(sessions: list[Path]) -> int:
    """Return the index of the focused session in `sessions`.

    Recovers gracefully if the focused session has disappeared (e.g.
    aged out of SESSION_FRESH_SECONDS, JSONL got deleted) by snapping
    to idx 0.
    """
    global _focused_session_path
    if not sessions:
        _focused_session_path = None
        return 0
    if _focused_session_path is None:
        _focused_session_path = sessions[0]
        return 0
    try:
        return sessions.index(_focused_session_path)
    except ValueError:
        _focused_session_path = sessions[0]
        return 0


def cycle_selected_session(direction: int = 1) -> int:
    """Advance the focus by +/-1 over the *path-identified* sessions."""
    global _focused_session_path
    sessions = find_active_sessions()
    if not sessions:
        return 0
    cur = _resolve_focus_index(sessions)
    new_idx = (cur + direction) % len(sessions)
    _focused_session_path = sessions[new_idx]
    log(f"Session cycle dir={direction:+d} → idx={new_idx} "
        f"of {len(sessions)}: {_focused_session_path.name}")
    return new_idx


async def read_session_payload() -> dict | None:
    """Build the per-session + multi-session payload."""
    sessions = find_active_sessions()
    if not sessions:
        return None
    focus_idx = _resolve_focus_index(sessions)
    focus = sessions[focus_idx]
    summary = read_session_usage(focus)
    if summary is None:
        return None
    try:
        mtime = focus.stat().st_mtime
    except OSError:
        mtime = 0.0
    # Primary signal: stop_reason="end_turn" on the last assistant
    # message means Claude declared the turn over — flip to idle even
    # if the mtime is still fresh, so the ESP32 swaps screens within a
    # poll tick of the response landing instead of waiting for the
    # SESSION_ACTIVE_SECONDS mtime timeout. Fall back to mtime-based
    # staleness when stop_reason isn't available (older transcripts,
    # tool_use waiting on results, etc).
    now_ts = time.time()
    is_active = (now_ts - mtime) < SESSION_ACTIVE_SECONDS
    if is_active and summary.get("last_stop_reason") == "end_turn":
        is_active = False
    settings_model = _settings_model() or ""
    label = _model_display_label(summary.get("model", ""), settings_model, summary["ctx_max"])

    # Compact summary for every active session — drives the overview UI.
    overview: list[dict] = []
    for s_path in sessions:
        s_sum = read_session_usage(s_path)
        if s_sum is None:
            continue
        try:
            s_mtime = s_path.stat().st_mtime
        except OSError:
            s_mtime = 0.0
        s_label = _model_display_label(s_sum.get("model", ""), settings_model, s_sum["ctx_max"])
        s_pct = 0
        if s_sum["ctx_max"]:
            s_pct = int(min(100.0, s_sum["ctx_used"] * 100.0 / s_sum["ctx_max"]) + 0.5)
        s_active = (now_ts - s_mtime) < SESSION_ACTIVE_SECONDS
        if s_active and s_sum.get("last_stop_reason") == "end_turn":
            s_active = False
        overview.append({
            "ml":  s_label,
            "cp":  s_pct,
            "ac":  1 if s_active else 0,
            "tt":  s_sum["task_tokens"],
        })

    # Determine API vs subscription, in priority order:
    #   1. JSONL `baseUrl` field (currently null in Claude Code 2.1.x,
    #      will become the source of truth once it's populated).
    #   2. ANTHROPIC_BASE_URL of the running claude process whose cwd
    #      matches this session.
    # If both signals are unavailable, leave base_url as None and let
    # _session_mode_for_base_url default to subscription — better to
    # show stale-looking subscription numbers than to guess a mode
    # from the model name (which is a misleading signal: Opus can
    # run on LiteLLM, Sonnet/Haiku can run on Max).
    base_url = summary.get("base_url")
    if not base_url:
        base_url = _base_url_for_cwd(summary.get("cwd"))

    return {
        # Stash base_url at the top level so poll_usage() can read it
        # without re-parsing the JSONL. Stripped before going over BLE.
        "_base_url": base_url,
        "cu": summary["ctx_used"],
        "cm": summary["ctx_max"],
        # `act=1` tells the firmware Claude Code is mid-turn → flip to the
        # working/animation screen. `act=0` returns to the usage screen.
        "act": 1 if is_active else 0,
        # Current task: output tokens since the last human user message,
        # and seconds elapsed since that boundary.
        "tt":  summary["task_tokens"],
        "ts":  summary["task_seconds"],
        # Model display label, e.g. "Opus 4.7 1M".
        "ml":  label,
        # Current phase — drives which working animation to play.
        "ph":  summary["phase"],
        # Multi-session: total count + the currently focused index
        # (computed via path lookup so it survives mtime re-sorting).
        "sn":  len(sessions),
        "si":  focus_idx,
        # Overview rows (kept short so the whole payload fits in one
        # BLE write; the firmware uses this for the overview screen).
        "sl":  overview,
    }


def _session_mode_for_base_url(base_url: str | None) -> str:
    """Empty/None baseUrl → talking to api.anthropic.com → subscription.
    Anything else (LiteLLM, Bedrock proxy, …) → API."""
    if not base_url:
        return "subscription"
    if "anthropic.com" in base_url.lower():
        return "subscription"
    return "api"


# ---- Per-session ANTHROPIC_BASE_URL discovery via /bin/ps -E ----
# Claude Code 2.1.x doesn't (yet) record the baseUrl field on JSONL
# records, so we read it straight out of each running `claude` process's
# environment. macOS `ps -E` exposes the env for our own user's procs;
# we map each PID's PWD to the running session's cwd and use that as
# the key.
_proc_env_cache: dict[str, tuple[str, float]] = {}     # cwd -> (base_url, ts)
_PROC_ENV_TTL = 3.0                                    # seconds


def _refresh_proc_env_cache() -> None:
    """Walk every running `claude` process, read its env (ps -E) and
    real-time cwd (lsof), and update the cwd → base_url cache.

    `PWD` in the env can be stale if the user cd'd inside the session;
    lsof's `cwd` is the kernel's view of the current directory and is
    always fresh.
    """
    import subprocess
    try:
        pids = subprocess.run(
            ["pgrep", "-x", "claude"],
            capture_output=True, text=True, timeout=2.0,
        ).stdout.split()
    except (subprocess.SubprocessError, FileNotFoundError):
        return
    now = time.time()
    _proc_env_cache.clear()
    for pid in pids:
        try:
            env_line = subprocess.run(
                ["ps", "-E", "-p", pid, "-ww", "-o", "command="],
                capture_output=True, text=True, timeout=1.0,
            ).stdout
        except (subprocess.SubprocessError, FileNotFoundError):
            continue
        base_url = ""
        for tok in env_line.split():
            if tok.startswith("ANTHROPIC_BASE_URL="):
                base_url = tok[len("ANTHROPIC_BASE_URL="):]
                break
        try:
            # -a ANDs -p and -d; without it lsof OR's them and returns
            # every fd from every process. -F outputs machine-parsable
            # lines: `p<pid>`, `n<name>`, etc.
            lsof_out = subprocess.run(
                ["lsof", "-a", "-p", pid, "-d", "cwd", "-Fn"],
                capture_output=True, text=True, timeout=1.0,
            ).stdout
        except (subprocess.SubprocessError, FileNotFoundError):
            continue
        cwd = ""
        for line in lsof_out.splitlines():
            if line.startswith("n"):
                cwd = line[1:].strip()
                break
        if cwd:
            _proc_env_cache[cwd] = (base_url, now)


def _base_url_for_cwd(cwd: str | None) -> str | None:
    """Return the ANTHROPIC_BASE_URL of the running claude process whose
    PWD matches `cwd`, refreshing the cache if it's stale."""
    if not cwd:
        return None
    now = time.time()
    cached = _proc_env_cache.get(cwd)
    if cached is None or (now - cached[1]) > _PROC_ENV_TTL:
        _refresh_proc_env_cache()
        cached = _proc_env_cache.get(cwd)
    return cached[0] if cached else None


async def poll_usage() -> dict | None:
    """Merge live session data with quota/billing data.

    The focused session's `baseUrl` decides which billing panel we push
    — subscription (5h / 7d windows) when the session is talking to
    api.anthropic.com directly, API (tokens / $ / burn) via LiteLLM
    when a proxy is in front of it.
    """
    session = await read_session_payload()
    focused_base_url = (session or {}).pop("_base_url", None) if session else None
    focus_mode = _session_mode_for_base_url(focused_base_url)
    log(f"Focus: ml={(session or {}).get('ml','?')} "
        f"base_url={focused_base_url!r} → {focus_mode}")

    base: dict | None = None
    if focus_mode == "api":
        cfg = load_litellm_config()
        if cfg is not None:
            base = await poll_api_billing(cfg)
        if base is None:
            # API session focused but no LiteLLM config (or poll
            # failed) → emit a stub so the device can still render the
            # API screen with "--" placeholders instead of locking us
            # back into the subscription panel.
            base = {"mode": "api", "tu": 0, "tq": 0, "ds": 0, "db": 0,
                    "bm": 0, "bc": 0, "ar": -1, "pl": "",
                    "st": "no_litellm", "ok": False}
    else:
        token = read_token()
        if not token:
            base = {"mode": "subscription", "s": 0, "sr": -1, "w": 0, "wr": -1,
                    "st": "no_token", "ok": False}
        else:
            base = await poll_subscription(token)

    if base is None and session is None:
        log("No quota source and no active session; skipping poll")
        return None
    if base is None:
        return {
            "mode": focus_mode,
            "s":  0.0, "sr": -1,
            "w":  0.0, "wr": -1,
            "st": "session",
            "ok": True,
            **session,
        }
    if session is None:
        return base
    # Merge: quota numbers win for the main rows, session fields win for
    # context + activity (and replace any cu/cm the api leg may have set).
    merged = dict(base)
    merged.update(session)
    return merged


class Session:
    def __init__(self, client: BleakClient) -> None:
        self.client = client
        self.refresh_requested = asyncio.Event()

    def _on_refresh(self, _char, data: bytearray) -> None:
        # Single-byte protocol: 0x01 = plain refresh request;
        # 0x02 = "next session" (device-side shake-to-switch);
        # 0x03 = "previous session". Anything else falls back to refresh.
        b = data[0] if data else 0x01
        if b == 0x02:
            new_idx = cycle_selected_session(+1)
            log(f"Device requested next session (now idx={new_idx})")
        elif b == 0x03:
            new_idx = cycle_selected_session(-1)
            log(f"Device requested previous session (now idx={new_idx})")
        else:
            log("Refresh requested by device")
        self.refresh_requested.set()

    async def setup_refresh_subscription(self) -> None:
        try:
            await self.client.start_notify(REQ_CHAR_UUID, self._on_refresh)
        except (BleakError, ValueError) as e:
            log(f"Refresh subscription unavailable: {e}")

    async def write_payload(self, payload: dict) -> bool:
        data = json.dumps(payload, separators=(",", ":")).encode()
        log(f"Sending: {data.decode()}")
        try:
            await self.client.write_gatt_char(RX_CHAR_UUID, data, response=False)
            return True
        except BleakError as e:
            log(f"Write failed: {e}")
            return False


async def connect_and_run(address: str, stop_event: asyncio.Event) -> bool:
    """Connect to a known address and poll until disconnected or stopped.

    Returns True if the connection was used successfully (so the caller
    keeps the cached address), False if the connection failed and the
    cache should be invalidated.
    """
    log(f"Connecting to {address}...")
    client = BleakClient(address)
    try:
        await client.connect()
    except (BleakError, asyncio.TimeoutError) as e:
        log(f"Connection failed: {e}")
        return False

    if not client.is_connected:
        log("Connection failed (no error but not connected)")
        return False

    log("Connected")
    session = Session(client)
    await session.setup_refresh_subscription()

    last_poll = 0.0
    last_jsonl_mtime = 0.0
    was_active: bool | None = None
    last_phase: str | None = None
    used_successfully = False
    try:
        while client.is_connected and not stop_event.is_set():
            # Push whenever (a) the device asked, (b) the active JSONL
            # changed, (c) the activity flag flipped (firmware needs to
            # know immediately so it can swap screens), (d) Claude Code
            # is mid-turn and the anchor interval elapsed, or (e) the
            # idle heartbeat interval has elapsed.
            now = time.time()
            should_push = (
                session.refresh_requested.is_set()
                or (now - last_poll) >= POLL_INTERVAL
            )
            active = find_active_session()
            session_is_active = False
            if active is not None:
                try:
                    cur_mtime = active.stat().st_mtime
                except OSError:
                    cur_mtime = 0.0
                if cur_mtime > last_jsonl_mtime:
                    last_jsonl_mtime = cur_mtime
                    should_push = True
                if (now - cur_mtime) < SESSION_ACTIVE_SECONDS:
                    session_is_active = True
                    # Tail-parse so we can see the latest stop_reason
                    # without re-doing the cold-parse work that
                    # read_session_payload would do moments later.
                    summary = read_session_usage(active)
                    if summary and summary.get("last_stop_reason") == "end_turn":
                        session_is_active = False
            # Active → idle (or idle → active) transition: push right
            # away so the firmware flips screens within a couple seconds
            # of the task finishing, not at the next 60s heartbeat.
            if was_active is not None and session_is_active != was_active:
                should_push = True
            was_active = session_is_active
            # Periodic anchor while active — keeps the firmware-side
            # timer from drifting if the JSONL isn't being touched.
            if session_is_active and (now - last_poll) >= ACTIVE_SYNC_INTERVAL:
                should_push = True

            if should_push:
                session.refresh_requested.clear()
                payload = await poll_usage()
                if payload is not None:
                    if await session.write_payload(payload):
                        last_poll = time.time()
                        used_successfully = True
                        last_phase = payload.get("ph", last_phase)

            try:
                await asyncio.wait_for(session.refresh_requested.wait(), timeout=TICK)
            except asyncio.TimeoutError:
                pass
    finally:
        try:
            await client.disconnect()
        except BleakError:
            pass

    log("Device disconnected" if not stop_event.is_set() else "Stopping")
    return used_successfully


async def main() -> None:
    stop_event = asyncio.Event()
    loop = asyncio.get_running_loop()

    def _stop(*_args: object) -> None:
        log("Daemon stopping")
        stop_event.set()

    for sig in (signal.SIGINT, signal.SIGTERM):
        try:
            loop.add_signal_handler(sig, _stop)
        except NotImplementedError:
            signal.signal(sig, _stop)

    log("=== Claude Usage Tracker Daemon (BLE, macOS) ===")
    log(f"Poll interval: {POLL_INTERVAL}s")

    backoff = 1
    while not stop_event.is_set():
        address = load_cached_address()
        if not address:
            address = await scan_for_device()
            if address:
                save_address(address)
            else:
                log(f"Device not found, retrying in {backoff}s...")
                try:
                    await asyncio.wait_for(stop_event.wait(), timeout=backoff)
                except asyncio.TimeoutError:
                    pass
                backoff = min(backoff * 2, 60)
                continue

        ok = await connect_and_run(address, stop_event)
        if not ok:
            log("Invalidating cached address")
            SAVED_ADDR_FILE.unlink(missing_ok=True)
            try:
                await asyncio.wait_for(stop_event.wait(), timeout=backoff)
            except asyncio.TimeoutError:
                pass
            backoff = min(backoff * 2, 60)
        else:
            backoff = 1


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        sys.exit(0)
