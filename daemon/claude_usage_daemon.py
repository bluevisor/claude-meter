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
TICK = 5
SCAN_TIMEOUT = 8.0
# When session is active, push at least this often so the on-device
# timer stays anchored — but the firmware advances seconds locally
# between pushes, so we don't need 1-Hz traffic.
ACTIVE_SYNC_INTERVAL = 15

# Claude Code transcript discovery.
CLAUDE_PROJECTS_DIR = Path.home() / ".claude" / "projects"
# A JSONL counts as "active" if its mtime is within this many seconds.
SESSION_FRESH_SECONDS = 30 * 60
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
        return None

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


# Rolling token history so we can compute burn rate. (last_value, last_ts).
_api_history: list[tuple[int, float]] = []


def _burn_rate(tokens_now: int, now_ts: float) -> tuple[int, int]:
    """Return (tokens_per_minute_instant, percent_change_vs_average)."""
    _api_history.append((tokens_now, now_ts))
    # Keep ~30 minutes of history (one poll/min → 30 entries)
    cutoff = now_ts - 30 * 60
    while len(_api_history) > 1 and _api_history[0][1] < cutoff:
        _api_history.pop(0)
    if len(_api_history) < 2:
        return 0, 0
    # Instantaneous: last minute or so
    prev_tok, prev_ts = _api_history[-2]
    dt = max(now_ts - prev_ts, 1.0)
    instant_per_min = int(round((tokens_now - prev_tok) * 60.0 / dt))
    # Average across the window
    first_tok, first_ts = _api_history[0]
    win = max(now_ts - first_ts, 60.0)
    avg_per_min = int(round((tokens_now - first_tok) * 60.0 / win))
    pct = 0
    if avg_per_min > 0:
        pct = int(round((instant_per_min - avg_per_min) * 100.0 / avg_per_min))
    return max(instant_per_min, 0), pct


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

    # Token counters — fall back to dollars/token conversion if LiteLLM
    # only exposes spend.
    dpt = float(cfg.get("dollars_per_token", 1e-6))
    tokens_used  = int(record.get("total_tokens", record.get("tokens_used", 0)) or 0)
    tokens_quota = int(record.get("token_budget", record.get("max_tokens",  0)) or 0)
    spend_dollars  = float(record.get("spend", 0.0) or 0.0)
    budget_dollars = float(record.get("max_budget", 0.0) or 0.0)
    if not tokens_used and spend_dollars and dpt > 0:
        tokens_used = int(spend_dollars / dpt)
    if not tokens_quota and budget_dollars and dpt > 0:
        tokens_quota = int(budget_dollars / dpt)

    burn_per_min, burn_pct = _burn_rate(tokens_used, time.time())

    now = datetime.now()
    period_label = cfg.get("period_label") or now.strftime("%b %Y")
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


def read_session_usage(jsonl: Path) -> dict | None:
    """Scan a JSONL transcript and summarize the current session."""
    last_usage: dict | None = None
    last_model: str | None = None
    last_ts: float | None = None
    first_ts: float | None = None
    total_input  = 0
    total_output = 0
    total_cache_read = 0
    total_cache_write = 0
    # "Current task" = since the most recent human user message. Tool
    # results show up as user-type entries too, so we filter them out.
    task_start_ts: float | None = None
    task_output_tokens: int = 0
    # Track the last two assistant content-block types + timestamp so we
    # can derive a "thinking" or "lightbulb" phase for the UI.
    prev_block_type: str | None = None
    last_block_type: str | None = None
    last_block_ts: float | None = None
    try:
        with jsonl.open("r", encoding="utf-8", errors="replace") as f:
            for line in f:
                line = line.strip()
                if not line:
                    continue
                try:
                    obj = json.loads(line)
                except json.JSONDecodeError:
                    continue
                t = obj.get("type")
                ts = _parse_iso_timestamp(obj.get("timestamp", ""))
                if t == "user":
                    msg = obj.get("message")
                    content = msg.get("content") if isinstance(msg, dict) else None
                    if _is_real_user_message(content):
                        # New task boundary — reset the running counters.
                        task_start_ts = ts
                        task_output_tokens = 0
                    continue
                if t != "assistant":
                    continue
                msg = obj.get("message")
                if not isinstance(msg, dict):
                    continue
                usage = msg.get("usage")
                if not isinstance(usage, dict):
                    continue
                if ts is not None:
                    if first_ts is None or ts < first_ts:
                        first_ts = ts
                    last_ts = ts
                last_usage = usage
                last_model = msg.get("model") or last_model
                out_tok = int(usage.get("output_tokens", 0) or 0)
                total_input  += int(usage.get("input_tokens", 0) or 0)
                total_output += out_tok
                total_cache_write += int(usage.get("cache_creation_input_tokens", 0) or 0)
                total_cache_read  += int(usage.get("cache_read_input_tokens", 0) or 0)
                if task_start_ts is not None and (ts is None or ts >= task_start_ts):
                    task_output_tokens += out_tok
                # Capture this message's first content-block type for the
                # phase heuristic. Claude Code writes one block per
                # assistant message in this transcript.
                content = msg.get("content")
                block_type: str | None = None
                if isinstance(content, list) and content:
                    head = content[0]
                    if isinstance(head, dict):
                        block_type = head.get("type")
                if block_type:
                    prev_block_type = last_block_type
                    last_block_type = block_type
                    last_block_ts = ts
    except OSError as e:
        log(f"Reading session JSONL failed: {e}")
        return None

    if last_usage is None:
        return None

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
          and last_block_ts and (now - last_block_ts) < 4):
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
    }


# If the JSONL hasn't been touched in this long, Claude Code is idle.
SESSION_ACTIVE_SECONDS = 60


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


# Index of the currently-selected session within find_active_sessions().
# Mutated by the BLE refresh char (shake-to-switch from the device).
_selected_session_idx = 0


def cycle_selected_session(direction: int = 1) -> int:
    """Advance the focus session by +/-1 and return the new index."""
    global _selected_session_idx
    n = max(1, len(find_active_sessions()))
    _selected_session_idx = (_selected_session_idx + direction) % n
    return _selected_session_idx


async def read_session_payload() -> dict | None:
    """Build the per-session + multi-session payload."""
    sessions = find_active_sessions()
    if not sessions:
        return None
    global _selected_session_idx
    if _selected_session_idx >= len(sessions):
        _selected_session_idx = 0
    focus = sessions[_selected_session_idx]
    summary = read_session_usage(focus)
    if summary is None:
        return None
    try:
        mtime = focus.stat().st_mtime
    except OSError:
        mtime = 0.0
    is_active = (time.time() - mtime) < SESSION_ACTIVE_SECONDS
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
        overview.append({
            "ml":  s_label,
            "cp":  s_pct,
            "ac":  1 if (time.time() - s_mtime) < SESSION_ACTIVE_SECONDS else 0,
            "tt":  s_sum["task_tokens"],
        })

    return {
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
        # Multi-session: total count + the currently focused index.
        "sn":  len(sessions),
        "si":  _selected_session_idx,
        # Overview rows (kept short so the whole payload fits in one
        # BLE write; the firmware uses this for the overview screen).
        "sl":  overview,
    }


async def poll_usage() -> dict | None:
    """Merge live session data with quota/billing data.

    Layering:
      * 5h / 7d / token / spend numbers come from LiteLLM (api mode)
        or the Anthropic OAuth rate-limit headers (subscription mode).
      * Context bar + activity flag come from the local JSONL.

    The session layer is best-effort — if there's no fresh transcript,
    we still emit the quota numbers.
    """
    base: dict | None = None
    cfg = load_litellm_config()
    if cfg is not None:
        base = await poll_api_billing(cfg)
    else:
        token = read_token()
        if token:
            base = await poll_subscription(token)

    session = await read_session_payload()

    if base is None and session is None:
        log("No quota source and no active session; skipping poll")
        return None
    if base is None:
        # No API/OAuth: emit a session-only payload so the meter still
        # shows the live context bar + working-mode flag.
        return {
            "mode": "subscription",
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
