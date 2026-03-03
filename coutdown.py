from datetime import datetime


now = datetime.now()
hour = now.hour
minute = now.minute
secoand = now.second
current_secoand = hour * 3600 + minute * 60 + secoand

taget_hour = 18
finish_hour = 6

on_secoands = taget_hour * 3600
off_secoands = finish_hour * 3600

if 6 <= hour < 18:
    target = on_secoands
    status = "🌞 Menuju Waktu Lamp ON"
else:
    status = "🌙 Menuju Waktu Lamp OFF"
    if hour >= 18:
        off_secoands += 24 * 3600
    target = off_secoands

remaining = target - current_secoand
if remaining < 0:
    remaining += 24 * 3600

hours = remaining // 3600
minutes = (remaining % 3600) // 60
seconds = remaining % 60

print(taget_hour)
print(finish_hour)
print( f"{hours} jam {minutes} menit {seconds} detik")
    