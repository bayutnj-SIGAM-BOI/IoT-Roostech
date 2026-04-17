import asyncio
import os
import time
import json
import tempfile
import io
import wave

import speech_recognition as sr
from gtts import gTTS

import discord
from discord.ext import commands
from paho.mqtt import client as mqtt_lib
from paho.mqtt.enums import CallbackAPIVersion

from config import (
    TOKEN_BOT, CHANNEL_ID,
    MQTT_SERVER, MQTT_PORT,
    MQTT_USERNAME, MQTT_PASSWORD,
    TOPIC_COMMAND, TOPIC_TELEMETRY, TOPIC_STATUS,
)

# ─── Konstanta reconnect ──────────────────────────────────────────────────────
FIRST_RECONNECT_DELAY = 1
RECONNECT_RATE        = 2
MAX_RECONNECT_COUNT   = 12
MAX_RECONNECT_DELAY   = 60

FLAG_EXIT = False

# ─── TTS Settings ─────────────────────────────────────────────────────────────
TTS_LANG = "id"
TTS_SLOW = False

# ─── Speech recognition ───────────────────────────────────────────────────────
recognizer = sr.Recognizer()

# ─── Discord bot ──────────────────────────────────────────────────────────────
intents = discord.Intents.default()
intents.message_content = True
intents.voice_states = True
bot = commands.Bot(command_prefix="!", intents=intents)

# ─── MQTT client ──────────────────────────────────────────────────────────────
client = mqtt_lib.Client(
    client_id="discord-bot",
    callback_api_version=CallbackAPIVersion.VERSION2,
)

# ─── Perintah suara → payload MQTT ────────────────────────────────────────────
# Sesuaikan "payload" dengan format yang ESP32 kamu harapkan
COMMAND_MAP = {
    "nyalakan lampu" : {"payload": '{"cmd":"led","val":1}',   "msg": "Baik, lampu dinyalakan"},
    "matikan lampu"  : {"payload": '{"cmd":"led","val":0}',   "msg": "Baik, lampu dimatikan"},
    "lampu nyala"    : {"payload": '{"cmd":"led","val":1}',   "msg": "Baik, lampu dinyalakan"},
    "lampu mati"     : {"payload": '{"cmd":"led","val":0}',   "msg": "Baik, lampu dimatikan"},
    "nyalakan relay" : {"payload": '{"cmd":"relay","val":1}', "msg": "Relay dinyalakan"},
    "matikan relay"  : {"payload": '{"cmd":"relay","val":0}', "msg": "Relay dimatikan"},
    "buka pintu"     : {"payload": '{"cmd":"servo","val":90}',"msg": "Baik, pintu dibuka"},
    "tutup pintu"    : {"payload": '{"cmd":"servo","val":0}', "msg": "Baik, pintu ditutup"},
}

# ─── MQTT callbacks ───────────────────────────────────────────────────────────
# FIX: CallbackAPIVersion.VERSION2 butuh 5 parameter (tambah reason_code & properties)
def on_connect(client, userdata, flags, reason_code, properties):
    if reason_code == 0:
        print("[MQTT] Terhubung ke broker!")
        client.subscribe(TOPIC_TELEMETRY, qos=1)
        client.subscribe(TOPIC_STATUS, qos=1)
        print(f"[MQTT] Subscribe: {TOPIC_TELEMETRY}, {TOPIC_STATUS}")
    else:
        print(f"[MQTT] Gagal terhubung, reason_code={reason_code}")


# FIX: on_disconnect VERSION2 juga butuh 5 parameter
def on_disconnect(client, userdata, flags, reason_code, properties):
    print(f"[MQTT] Terputus, reason_code={reason_code}")

    # FIX: pakai satu nama variabel yang konsisten (reconnect_count)
    reconnect_count = 0
    reconnect_delay = FIRST_RECONNECT_DELAY

    while reconnect_count < MAX_RECONNECT_COUNT:
        print(f"[MQTT] Reconnect dalam {reconnect_delay} detik...")
        time.sleep(reconnect_delay)
        try:
            client.reconnect()
            print("[MQTT] Reconnect berhasil!")
            return
        except Exception as err:
            print(f"[MQTT] Reconnect gagal: {err}")

        reconnect_delay  = min(reconnect_delay * RECONNECT_RATE, MAX_RECONNECT_DELAY)
        reconnect_count += 1

    print(f"[MQTT] Gagal reconnect setelah {MAX_RECONNECT_COUNT} percobaan. Keluar.")
    global FLAG_EXIT
    FLAG_EXIT = True


def on_message(client, userdata, message):
    """Terima pesan dari TOPIC_TELEMETRY / TOPIC_STATUS."""
    topic   = message.topic
    payload = message.payload.decode()
    print(f"[MQTT] <- [{topic}]: {payload}")


# ─── gTTS: teks ke file MP3 ───────────────────────────────────────────────────
def tts_to_mp3(text: str) -> str:
    tts = gTTS(text=text, lang=TTS_LANG, slow=TTS_SLOW)
    tmp = tempfile.NamedTemporaryFile(suffix=".mp3", delete=False)
    tmp_path = tmp.name
    tmp.close()
    tts.save(tmp_path)
    return tmp_path


# FIX: nama fungsi diseragamkan jadi speak_in_channel (sebelumnya voice_in_channel)
async def speak_in_channel(vc: discord.VoiceClient, text: str):
    if not vc or not vc.is_connected():
        return

    print(f"[TTS] Berbicara: \"{text}\"")

    mp3_path = await asyncio.get_event_loop().run_in_executor(
        None, tts_to_mp3, text
    )

    # FIX: is_playing adalah method, harus dipanggil dengan () bukan property
    while vc.is_playing():
        await asyncio.sleep(0.2)

    def after_play(error):
        if error:
            print(f"[TTS] Error saat memutar: {error}")
        try:
            os.unlink(mp3_path)
        except Exception:
            pass

    if vc.is_connected():
        vc.play(discord.FFmpegPCMAudio(mp3_path), after=after_play)


# ─── MQTT publish ─────────────────────────────────────────────────────────────
def mqtt_publish(payload: str):
    result = client.publish(TOPIC_COMMAND, payload, qos=1)
    print(f"[MQTT] -> [{TOPIC_COMMAND}]: {payload}  (rc={result.rc})")


# ─── Parse perintah suara ─────────────────────────────────────────────────────
def parse_command(text: str) -> dict | None:
    text_lower = text.lower().strip()
    for keyword, command in COMMAND_MAP.items():
        if keyword in text_lower:
            return {**command, "keyword": keyword, "raw_text": text}
    return None


# ─── Speech-to-Text ───────────────────────────────────────────────────────────
def transcribe_audio(audio_data: bytes) -> str:
    wav_buffer = io.BytesIO()
    with wave.open(wav_buffer, "wb") as wf:
        wf.setnchannels(2)
        wf.setsampwidth(2)
        wf.setframerate(48000)
        wf.writeframes(audio_data)
    wav_buffer.seek(0)

    with sr.AudioFile(wav_buffer) as source:
        audio = recognizer.record(source)

    try:
        return recognizer.recognize_google(audio, language="id-ID")
    except sr.UnknownValueError:
        return ""
    except sr.RequestError as e:
        print(f"[STT Error] {e}")
        return ""


# ─── VoiceSink ────────────────────────────────────────────────────────────────
# class VoiceSink(discord.AudioSink):
#     def __init__(self, callback):
#         self.callback   = callback
#         self._buffers: dict[int, bytearray] = {}
#         self._THRESHOLD = 48000 * 2 * 2 * 2  # ~2 detik audio

#     def write(self, user, data):
#         if user is None:
#             return
#         uid = user.id
#         if uid not in self._buffers:
#             self._buffers[uid] = bytearray()
#         self._buffers[uid].extend(data.data)

#         if len(self._buffers[uid]) >= self._THRESHOLD:
#             audio_bytes = bytes(self._buffers[uid])
#             self._buffers[uid].clear()
#             asyncio.create_task(self.callback(user, audio_bytes))

#     def cleanup(self):
#         self._buffers.clear()

class Sinks(enumerate):
    mp3 = discord.sinks.MP3Sink()
    wav = discord.sinks.WaveSink()
    pcm = discord.sinks.PCMSink()
    ogg = discord.sinks.OGGSink()

async def VoiceSink(ctx: discord.ApplicationContext, sink: Sinks):
    """Records voice from the channel."""
    if ctx.voice_client is None:
        await ctx.author.voice.channel.connect() # Connect if not already connected

    # Start listening with the selected sink
    ctx.voice_client.listen(sink.value) 
    await ctx.respond(f"Recording to {sink.name} started!")


# ─── Discord events & commands ────────────────────────────────────────────────
@bot.event
async def on_ready():
    print(f"[Bot] Login sebagai {bot.user} (ID: {bot.user.id})")
    # FIX: TTS_LANG dan TTS_SLOW sekarang didefinisikan di atas
    print(f"[TTS] gTTS | lang={TTS_LANG} | slow={TTS_SLOW}")
    print("[Bot] Ketik !join di channel untuk mulai mendengarkan suara.")


@bot.command(name="join")
async def join(ctx: commands.Context):
    if ctx.author.voice is None:
        await ctx.send("Kamu harus berada di voice channel dulu!")
        return

    channel = ctx.author.voice.channel
    if ctx.voice_client:
        await ctx.voice_client.disconnect()

    vc = await channel.connect()
    await ctx.send(f"Bergabung ke **{channel.name}** - siap menerima perintah suara!")
    await speak_in_channel(vc, "Halo! Saya siap menerima perintah. Silakan bicara.")

    async def on_audio(user, audio_bytes):
        if user.bot:
            return

        text = await asyncio.get_event_loop().run_in_executor(
            None, transcribe_audio, audio_bytes
        )
        if not text:
            return

        print(f"[STT] {user.display_name}: \"{text}\"")

        command = parse_command(text)
        if command:
            await ctx.send(
                f"**{user.display_name}**: \"{text}\"\n"
                f"Menjalankan: `{command['keyword']}`"
            )

            # FIX: kirim via MQTT pakai mqtt_publish (bukan send_to_esp32_http/send_to_esp32_mqtt)
            mqtt_publish(command["payload"])
            await ctx.send(f"{command['msg']}")
            await speak_in_channel(vc, command["msg"])
        else:
            print(f"[Bot] Bukan perintah: \"{text}\"")

    vc.start_recording(VoiceSink(on_audio), lambda e: print(f"[VC Error] {e}"))


@bot.command(name="leave")
async def leave(ctx: commands.Context):
    if ctx.voice_client:
        await speak_in_channel(ctx.voice_client, "Sampai jumpa!")
        await asyncio.sleep(2)
        ctx.voice_client.stop_recording()
        await ctx.voice_client.disconnect()
        await ctx.send("Keluar dari voice channel.")
    else:
        await ctx.send("Bot tidak sedang di voice channel.")


@bot.command(name="say")
async def say(ctx: commands.Context, *, text: str):
    if ctx.voice_client is None:
        await ctx.send("Bot belum di voice channel. Ketik `!join` dulu.")
        return
    await speak_in_channel(ctx.voice_client, text)
    await ctx.send(f"Bot mengucapkan: \"{text}\"")


@bot.command(name="tts")
async def tts_settings(ctx: commands.Context, lang: str = None, slow: str = None):
    global TTS_LANG, TTS_SLOW
    changed = []
    if lang is not None:
        TTS_LANG = lang
        changed.append(f"lang={TTS_LANG}")
    if slow is not None:
        TTS_SLOW = slow.lower() in ("slow", "true", "1", "yes")
        changed.append(f"slow={TTS_SLOW}")

    if changed:
        await ctx.send(f"TTS diperbarui: {', '.join(changed)}")
        if ctx.voice_client:
            await speak_in_channel(ctx.voice_client, "Pengaturan suara diperbarui.")
    else:
        await ctx.send(
            f"Pengaturan gTTS:\n"
            f"- Bahasa: `{TTS_LANG}`\n"
            f"- Lambat: `{TTS_SLOW}`\n\n"
            f"Contoh: `!tts id` atau `!tts en slow`"
        )


@bot.command(name="perintah")
async def list_commands(ctx: commands.Context):
    lines = ["**Perintah Suara:**"]
    for kw, val in COMMAND_MAP.items():
        lines.append(f"- `{kw}` -> {val['msg']}")
    await ctx.send("\n".join(lines))


# ─── Entry point ──────────────────────────────────────────────────────────────
if __name__ == "__main__":
    # FIX: setup MQTT lengkap sebelum bot.run()
    client.username_pw_set(MQTT_USERNAME, MQTT_PASSWORD)
    client.on_connect    = on_connect
    client.on_disconnect = on_disconnect
    client.on_message    = on_message

    client.connect(MQTT_SERVER, MQTT_PORT, keepalive=60)
    client.loop_start()  # jalankan MQTT di background thread

    try:
        bot.run(TOKEN_BOT)
    finally:
        client.loop_stop()
        client.disconnect()