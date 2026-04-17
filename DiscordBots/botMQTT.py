import asyncio
import threading
import time
import json
import ssl
import socket

import discord
from discord.ext import commands
from paho.mqtt import client as mqtt_lib
from paho.mqtt.enums import CallbackAPIVersion

from config import (
    TOKEN_BOT,
    CHANNEL_ID,
    MQTT_SERVER,
    MQTT_PORT,
    MQTT_USERNAME,
    MQTT_PASSWORD,
    TOPIC_COMMAND,
    TOPIC_TELEMETRY,
    TOPIC_STATUS,
)

# ── Reconnect config ─────────────────────────────────────────
FIRST_RECONNECT_DELAY = 1
RECONNECT_RATE        = 2
MAX_RECONNECT_COUNT   = 12
MAX_RECONNECT_DELAY   = 60

FLAG_EXIT = False

# ── Discord & MQTT init ──────────────────────────────────────
intents = discord.Intents.default()
intents.message_content = True
bot = commands.Bot(command_prefix="!", intents=intents)

# client_id unik pakai timestamp, hindari konflik sesi ganda
client = mqtt_lib.Client(
    client_id=f"discord-bot-{int(time.time())}",
    callback_api_version=CallbackAPIVersion.VERSION2,
)

# Simpan referensi pesan telemetri per channel agar bisa di-edit
_telemetry_msg: dict[int, discord.Message] = {}


# ════════════════════════════════════════════════════════════
#  MQTT CALLBACKS
# ════════════════════════════════════════════════════════════

def on_connect(client, userdata, flags, rc, properties=None):
    if rc == 0:
        print("✅ Connected to MQTT Server!")
        client.subscribe(TOPIC_TELEMETRY, qos=1)
        client.subscribe(TOPIC_STATUS, qos=1)
        print(f"📡 Subscribed: {TOPIC_TELEMETRY}, {TOPIC_STATUS}")
    else:
        print(f"❌ Failed to connect, rc={rc}")


def on_disconnect(client, userdata, flags, rc, properties=None):
    # Paho MQTT v2 (CallbackAPIVersion.VERSION2) butuh 5 parameter
    print(f"🔌 Disconnected (rc={rc}), mencoba reconnect...")
    reconnect_count = 0
    reconnect_delay = FIRST_RECONNECT_DELAY

    while reconnect_count < MAX_RECONNECT_COUNT:
        print(f"  ↩ Percobaan {reconnect_count + 1} dalam {reconnect_delay}s...")
        time.sleep(reconnect_delay)
        try:
            client.reconnect()
            print("✅ Reconnected!")
            return
        except Exception as err:
            print(f"  ⚠️  {err}")
        reconnect_delay = min(reconnect_delay * RECONNECT_RATE, MAX_RECONNECT_DELAY)
        reconnect_count += 1

    print("❌ Semua percobaan reconnect gagal.")
    global FLAG_EXIT
    FLAG_EXIT = True


def on_message(client, userdata, msg):
    """Terima pesan dari ESP32, parse JSON, kirim/edit ke channel Discord."""
    topic   = msg.topic
    payload = msg.payload.decode("utf-8")
    print(f"📨 MQTT [{topic}]: {payload}")

    try:
        data    = json.loads(payload)
        content = _format_message(topic, data)
    except (json.JSONDecodeError, ValueError):
        content = f"📟 **ESP32** `[{topic}]`\n```\n{payload}\n```"
        asyncio.run_coroutine_threadsafe(_send_to_discord(content), bot.loop)
        return

    # Telemetri biasa → edit pesan yang sudah ada (1 kotak)
    # Event lain (status, pong, feeding_done) → tetap kirim pesan baru
    is_telemetry = (
        topic == TOPIC_TELEMETRY
        and not any(k in data for k in ("event", "response", "state"))
    )

    coro = _edit_or_send_telemetry(content) if is_telemetry else _send_to_discord(content)
    asyncio.run_coroutine_threadsafe(coro, bot.loop)


# ════════════════════════════════════════════════════════════
#  FORMATTER
# ════════════════════════════════════════════════════════════

def _format_message(topic: str, data: dict) -> str:
    """Ubah payload MQTT → pesan Discord yang rapi & informatif."""

    # ── Status Online / Offline ──────────────────────────────
    if topic == TOPIC_STATUS:
        state = data.get("state", "unknown").upper()
        emoji = "🟢" if state == "ONLINE" else "🔴"
        return f"{emoji} **ESP32 Status:** `{state}`"

    # ── Event khusus dari ESP32 ──────────────────────────────
    event = data.get("event")

    if event == "feeding_done":
        dur = data.get("duration", "?")
        return (
            f"✅ **Feeding selesai!**\n"
            f"  🍽️ Servo kembali ke posisi **90° (tutup)**\n"
            f"  ⏱️ Durasi: `{dur} detik`"
        )

    if data.get("response") == "pong":
        temp = data.get("temperature", "?")
        rssi = data.get("rssi", "?")
        lamp = data.get("lamp", "?")
        pump = data.get("pump", "?")
        return (
            f"🏓 **Pong dari ESP32!**\n"
            f"  🌡️ Suhu: `{temp} °C`\n"
            f"  📶 RSSI: `{rssi} dBm`\n"
            f"  💡 Lampu: `{lamp}` | 🚿 Pump: `{pump}`"
        )

    # ── Telemetri biasa ──────────────────────────────────────
    lines = ["📊 **Telemetry ESP32**"]
    for k, v in data.items():
        emoji = _emoji_for(k)
        unit  = _unit_for(k)
        lines.append(f"  {emoji} **{k}**: `{v}{unit}`")
    return "\n".join(lines)


def _unit_for(key: str) -> str:
    # 'humadity' typo sengaja agar cocok dengan payload ESP32
    return {
        "temperature": " °C",
        "humadity":    " %",
        "rssi":        " dBm",
    }.get(key.lower(), "")


def _emoji_for(key: str) -> str:
    return {
        "temperature": "🌡️",
        "humadity":    "💧",
        "rssi":        "📶",
        "lamp":        "💡",
        "pump":        "🚿",
    }.get(key.lower(), "•")


# ════════════════════════════════════════════════════════════
#  DISCORD HELPERS
# ════════════════════════════════════════════════════════════

async def _send_to_discord(content: str):
    """Kirim pesan baru ke channel."""
    channel = bot.get_channel(CHANNEL_ID)
    if channel:
        await channel.send(content)
    else:
        print(f"⚠️  Channel {CHANNEL_ID} tidak ditemukan")


async def _edit_or_send_telemetry(content: str):
    """Edit pesan telemetri sebelumnya; kirim baru jika belum ada."""
    channel = bot.get_channel(CHANNEL_ID)
    if not channel:
        print(f"⚠️  Channel {CHANNEL_ID} tidak ditemukan")
        return

    existing = _telemetry_msg.get(CHANNEL_ID)
    if existing:
        try:
            await existing.edit(content=content)
            return
        except discord.NotFound:
            # Pesan sudah dihapus manual → hapus referensi, kirim baru
            _telemetry_msg.pop(CHANNEL_ID, None)
        except discord.Forbidden:
            print("⚠️  Bot tidak punya izin edit pesan")
            return

    msg = await channel.send(content)
    _telemetry_msg[CHANNEL_ID] = msg


def mqtt_publish(topic: str, payload: str):
    result = client.publish(topic, payload, qos=1)
    print(f"📤 Publish [{topic}]: {payload}  (rc={result.rc})")


# ════════════════════════════════════════════════════════════
#  DISCORD EVENTS & COMMANDS
# ════════════════════════════════════════════════════════════

@bot.event
async def on_ready():
    print(f"🤖 Bot {bot.user} online!")
    channel = bot.get_channel(CHANNEL_ID)
    if channel:
        await channel.send(
            "🤖 **Bot Aktif!** Ketik `!help` untuk daftar perintah.\n"
            "⚠️ Gunakan semua perintah di channel `#iot-control`."
        )


@bot.remove_command("help")
@bot.command(name="help")
async def cmd_help(ctx):
    embed = discord.Embed(
        title="🤖 ESP32 Controller Bot",
        description="Kontrol kandang/kebun kamu via Discord!",
        color=discord.Color.blurple(),
    )
    embed.add_field(
        name="💡 `!relay 1 on|off`",
        value=(
            "Kontrol **Lampu** secara manual.\n"
            "*Auto: nyala saat 20°C ≤ suhu < 30°C, mati saat ≥ 30°C*"
        ),
        inline=False,
    )
    embed.add_field(
        name="🚿 `!relay 2 on|off`",
        value="Kontrol **Water Pump** secara manual.",
        inline=False,
    )
    embed.add_field(
        name="🍽️ `!feed start <detik>`",
        value="Buka servo feeder selama N detik, lalu tutup otomatis.\n*Contoh: `!feed start 5`*",
        inline=False,
    )
    embed.add_field(name="🏓 `!ping`",   value="Cek respons & status ESP32.",     inline=False)
    embed.add_field(name="📡 `!status`", value="Cek status koneksi MQTT broker.", inline=False)
    embed.set_footer(text=f"MQTT Broker: {MQTT_SERVER}:{MQTT_PORT}")
    await ctx.send(embed=embed)


@bot.command(name="status")
async def cmd_status(ctx):
    state = "🟢 Connected" if client.is_connected() else "🔴 Disconnected"
    await ctx.send(
        f"📡 **MQTT Status:** {state}\n"
        f"🖥️ Server: `{MQTT_SERVER}:{MQTT_PORT}`"
    )


@bot.command(name="ping")
async def cmd_ping(ctx):
    mqtt_publish(TOPIC_COMMAND, json.dumps({"command": "ping"}))
    await ctx.send("🏓 Ping dikirim ke ESP32, tunggu respons...")


@bot.command(name="feed")
async def cmd_feed(ctx, state: str = "", duration: int = 0):  # ← NO indent di sini
    """Penggunaan: !feed start <detik>  |  Contoh: !feed start 5"""
    if state.lower() != "start" or duration <= 0:
        await ctx.send(
            "⚠️ Penggunaan: `!feed start <detik>`\n"
            "Contoh: `!feed start 5`"
        )
        return

    payload = json.dumps({"command": "servo", "value": "start", "duration": duration})
    mqtt_publish(TOPIC_COMMAND, payload)

    msg = await ctx.send(
        f"🍽️ Feeding dimulai! Servo → **180°**\n"
        f"⏳ **{duration} detik lagi...**"
    )

    for remaining in range(duration - 1, 0, -1):
        await asyncio.sleep(1)
        await msg.edit(
            content=(
                f"🍽️ Feeding berjalan... Servo → **180°**\n"
                f"⏳ **{remaining} detik lagi...**"
            )
        )

    await asyncio.sleep(1)
    await msg.edit(content="✅ **Feeding selesai!** Servo kembali ke **90°**")


@bot.command(name="relay")
async def cmd_relay(ctx, pin: str = "", state: str = ""):
    """
    Penggunaan: !relay <pin> on|off
    Pin 1 = Lampu, Pin 2 = Water Pump
    """
    state_lower = state.lower()
    if not pin or state_lower not in ("on", "off"):
        await ctx.send(
            "⚠️ Penggunaan: `!relay <pin> on|off`\n"
            "`pin 1` = 💡 Lampu | `pin 2` = 🚿 Water Pump\n"
            "Contoh: `!relay 1 on`"
        )
        return

    label = {"1": "💡 Lampu", "2": "🚿 Water Pump"}.get(pin, f"Pin {pin}")
    payload = json.dumps({"command": "relay", "pin": pin, "value": state_lower})
    mqtt_publish(TOPIC_COMMAND, payload)
    await ctx.send(f"⚡ {label} → **{state_lower.upper()}**")


# ════════════════════════════════════════════════════════════
#  MQTT SETUP
# ════════════════════════════════════════════════════════════

def setup_mqtt() -> None:
    client.username_pw_set(MQTT_USERNAME, MQTT_PASSWORD)

    tls_ctx = ssl.create_default_context()
    client.tls_set_context(tls_ctx)

    client.on_connect    = on_connect
    client.on_disconnect = on_disconnect
    client.on_message    = on_message

    if not MQTT_SERVER:
        raise ValueError("❌ MQTT_SERVER kosong! Cek config.py")

    print(f"🔌 Connecting ke {MQTT_SERVER}:{MQTT_PORT}…")
    try:
        client.connect(MQTT_SERVER, MQTT_PORT, keepalive=60)
    except socket.gaierror as e:
        print(f"❌ Gagal resolve hostname '{MQTT_SERVER}': {e}")
        print("   → Cek koneksi internet & nilai MQTT_SERVER di config.py")
        raise SystemExit(1)
    except ConnectionRefusedError:
        print(f"❌ Koneksi ditolak ke {MQTT_SERVER}:{MQTT_PORT}")
        print("   → Cek MQTT_PORT dan TLS config")
        raise SystemExit(1)
    except Exception as e:
        print(f"❌ Koneksi MQTT gagal: {e}")
        raise SystemExit(1)

    t = threading.Thread(target=client.loop_forever, daemon=True)
    t.start()
    print("🔄 MQTT loop thread berjalan")


# ════════════════════════════════════════════════════════════
#  ENTRYPOINT
# ════════════════════════════════════════════════════════════

if __name__ == "__main__":
    setup_mqtt()
    bot.run(TOKEN_BOT)