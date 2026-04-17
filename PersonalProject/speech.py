import speech_recognition as sr
import pyttsx3


# ─── Setup ───────────────────────────────────────────────
recognizer = sr.Recognizer()
engine = pyttsx3.init()

# Atur kecepatan & volume suara (opsional)
engine.setProperty('rate', 160)    # kecepatan bicara (default ~200)
engine.setProperty('volume', 1.0)  # volume 0.0 – 1.0

def speak(text):
    """Text-to-speech: AI ngomong balik ke kamu."""
    print(f"AI  : {text}")
    engine.say(text)
    engine.runAndWait()

def listen():
    """Dengerin input suara dari mic, kembalikan teks."""
    with sr.Microphone() as source:
        print("\n[🎤 Dengerin...] Silakan ngomong!")
        recognizer.adjust_for_ambient_noise(source, duration=0.5)
        try:
            audio = recognizer.listen(source, timeout=5, phrase_time_limit=8)
        except sr.WaitTimeoutError:
            return None

    try:
        # Ganti language="id-ID" buat Bahasa Indonesia
        text = recognizer.recognize_google(audio, language="id-ID")
        print(f"Kamu: {text}")
        return text.lower()
    except sr.UnknownValueError:
        print("[!] Nggak kedengeran jelas.")
        return None
    except sr.RequestError as e:
        print(f"[!] Error Google STT: {e}")
        return None

def respond(text):
    """Logika jawaban sederhana berdasarkan kata kunci."""
    if text is None:
        speak("Maaf, aku nggak denger. Coba lagi ya.")
        return True  # tetap lanjut loop

    # ── Keluar ──
    if any(w in text for w in ["keluar", "exit", "berhenti", "stop", "quit"]):
        speak("Oke, sampai jumpa!")
        return False  # hentikan loop

    # ── Sapaan ──
    elif any(w in text for w in ["halo", "hai", "hello", "hi", "selamat pagi",
                                  "selamat siang", "selamat sore", "selamat malam"]):
        speak("Halo! Ada yang bisa aku bantu?")

    # ── Nama ──
    elif any(w in text for w in ["siapa kamu", "nama kamu", "kamu siapa"]):
        speak("Aku adalah asisten suara sederhana buatan Python. Senang berkenalan!")

    # ── Kabar ──
    elif any(w in text for w in ["apa kabar", "gimana kabar", "how are you"]):
        speak("Aku baik-baik aja, makasih udah nanya! Kamu sendiri gimana?")

    # ── Waktu ──
    elif any(w in text for w in ["jam berapa", "sekarang jam", "waktu sekarang"]):
        from datetime import datetime
        now = datetime.now().strftime("%H:%M")
        speak(f"Sekarang jam {now}.")

    # ── Tanggal ──
    elif any(w in text for w in ["tanggal berapa", "hari ini tanggal", "hari apa"]):
        from datetime import datetime
        hari = ["Senin","Selasa","Rabu","Kamis","Jumat","Sabtu","Minggu"]
        now = datetime.now()
        speak(f"Hari ini {hari[now.weekday()]}, {now.strftime('%d %B %Y')}.")

    # ── Terima kasih ──
    elif any(w in text for w in ["terima kasih", "makasih", "thanks"]):
        speak("Sama-sama! Senang bisa membantu.")

    # ── Default ──
    else:
        speak(f"Kamu bilang: {text}. Maaf, aku belum bisa jawab itu.")

    return True  # lanjut loop

# ─── Main Loop ───────────────────────────────────────────
if __name__ == "__main__":
    speak("Halo! Aku siap mendengarkan. Katakan 'keluar' untuk berhenti.")

    running = True
    while running:
        text = listen()
        running = respond(text)