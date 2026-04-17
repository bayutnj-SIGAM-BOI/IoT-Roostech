import discord
import asyncio
import serial
import threading
import os
import time
from notifypy import Notify
from dotenv import load_dotenv
from discord.ext import commands
from datetime import datetime

load_dotenv()

TOKEN = os.getenv("TOKEN_BOT")
CHANNEL_ID = 1478371112646606910
CONTROL_ID_CHANNEL = 1475857713002188892
SERIAL_PORT = ""
BAUD_RATE = 115200

intents = discord.Intents.default()
intents.message_content = True

bot = commands.Bot(command_prefix="!", intents=intents)

current_servo = 90
current_status_servo = "CLOSE"
current_pump = "OFF"
current_lamp = "OFF"
current_temp = 24
MAX_TEMP = 29
AUTO_LAMP = True
status_message = None

Alert_Cooldown = 60
Last_Alert = 0

ON = "ON"
OFF = "OFF"

try:
    ser = serial.Serial(SERIAL_PORT, BAUD_RATE)
    print("Serial Connected")
except:
    ser = None
    print("Serial Not Connected")

def sendData(command : str, value):
    if ser is None:
        print("Serial isn't connect")
        return
    
    message = f"{command}:{value}\n"
    ser.write(message.encode())
    print("Data diprocess -> ESP32", message.strip())


@bot.event
async def on_ready():
    global status_message;

    print(f"Bot {bot.user} Telah Online".format(bot.user))
    bot.loop.create_task(AutoLamp_Time())

    channel = bot.get_channel(CHANNEL_ID)
    if channel:
        embed = discord.Embed(title="ESP32 Status", description="Arduino | Bot sudah Online", color=discord.Color.green())

        embed.add_field(name="🔄 Servo", value="!servo (OPEN/CLOSE)", inline=False)
        embed.add_field(name="💧 Pump", value="!pump ON/OFF", inline=False)
        embed.add_field(name="💡 Lamp",value="!lamp ON/OFF",inline=False)
        embed.add_field(name="🌡️ Temperature",value="!temp 20-40",inline=False)
        embed.add_field(name="🔄 System Reset", value="!defaultSet", inline=False)
        
        await channel.send(embed=embed)


# ========= CONTROL DISCORD SYSTEM =========
@bot.command()
async def servo(ctx, state: str):
    global current_servo
    global current_status_servo
    state = state.upper()
    # if 90 <= degree <= 180:
    #     current_servo = degree
    #     sendData("Servo", degree)

    if state == "OPEN" :
        current_servo = 90
        current_status_servo = "OPEN"
        sendData("Servo", 90)
        embed = discord.Embed(title="Servo Status", description=f"Servo diputar ke {current_servo}º", color=discord.Color.blue())
        await ctx.send(embed=embed)
    elif state == "CLOSE":
        current_servo = 180
        current_status_servo = "OFF"
        sendData("Servo", 180)
        embed = discord.Embed(title="Servo Status", description=f"Servo diputar ke {current_servo}º", color=discord.Color.red())
        await ctx.send(embed=embed)
    else:
        await ctx.send("Gunakkan !servo OPEN / !servo CLOSE")

@bot.command()
async def pump(ctx, state: str):
    global current_pump
    state = state.upper()

    if state in ["ON", "OFF"]:
        current_pump = state
        sendData("Pump", state)
        embed = discord.Embed(title="Pump Status", description=f"Pump {state}", color=discord.Color.green() if state == "ON" else discord.Color.red())
        await ctx.send(embed=embed)
    else:
        await ctx.send(f"Gunakaan /pump {ON}/{OFF}")

@bot.command()
async def lamp(ctx, state: str):
    global current_lamp
    state = state.upper()

    if state in ["ON", "OFF"]:
        current_lamp = state
        sendData("Lamp", state)
        embed = discord.Embed(title="💡 Lamp Status", description=f"Lamp {state}", color=discord.Color.green() if state == "ON" else discord.Color.red())
        await ctx.send(embed=embed)
    else:
        await ctx.send(f"Gunakaan /lamp {ON}/{OFF}")

@bot.command()
async def temp(ctx, value : int):
    global current_temp

    if 20 <= value <= 40:
        current_temp = value
        sendData("Temperature", value)

        embed = discord.Embed(title="🌡️ Temperature Status", description=f"Temperature {value}º", color=discord.Color.red())
        await ctx.send(embed=embed)
    else:
        await ctx.send("Masukkan Temperature 20-40º")

def countdown():
    now = datetime.now()
    hour = now.hour
    minute = now.minute
    secoand = now.second
    current_secoand = hour * 3600 + minute * 60 + secoand

    on_secoands = 18 * 3600
    off_secoands = 6 * 3600

    if 6 <= hour < 18:
        target = on_secoands
        status = "🌞 Menuju Waktu Lamp ON"
    else:
        status = "🌙 Menuju Waktu Lamp OFF"
        if hour >= 18:
            off_secoands += 24 * 3600
        target = off_secoands

    error = target - current_secoand
    if error < 0:
        error += 24 * 3600

    hours = error // 3600
    minutes = (error % 3600) // 60
    seconds = error % 60
    
    return status, f"{hours} jam {minutes} menit {seconds} detik"

async def AutoLamp_Time():
    await bot.wait_until_ready()
    global current_lamp
    channel = bot.get_channel(CHANNEL_ID)

    while not bot.is_closed():
        if AUTO_LAMP and channel:
            
            now = datetime.now()
            hour = now.hour

            if hour >= 18 or hour < 6:
                if current_lamp != "ON":
                    current_lamp = "ON"
                    sendData("Lamp", "ON")
                    embed = discord.Embed(title="🌙 Auto Lamp ON", color=discord.Color.green())
                    await channel.send(embed=embed)
            else:
                if current_lamp != "OFF":
                    current_lamp = "OFF"
                    sendData("Lamp", "OFF")
                    embed = discord.Embed(title="🌙 Auto Lamp OFF", color=discord.Color.red())
                    await channel.send(embed=embed)
        await asyncio.sleep(60)

@bot.command()
async def defaultSet(ctx):
    global current_lamp, current_pump, current_servo, current_temp

    current_lamp = "OFF"
    current_pump = "OFF"
    current_servo = 90
    current_temp = 20

    sendData("Servo", 90)
    sendData("Pump", "OFF")
    sendData("Lamp", "OFF")
    sendData("Temperature", 20)

    embed = discord.Embed( title="🔄 System Reset", description="Semua setting dikembalikan ke default", color=discord.Color.orange())
    embed.add_field(name="Servo", value="90º", inline=True)
    embed.add_field(name="Pump", value="OFF", inline=True)
    embed.add_field(name="Lamp", value="OFF", inline=True)
    embed.add_field(name="Temperature", value="20º", inline=True)
    
    await ctx.send(embed=embed)

@bot.command()
async def info(ctx):
    status, time_left = countdown()

    channel = bot.get_channel(CHANNEL_ID)
    embed = discord.Embed(title="📊 ESP32 Current Status", color=discord.Color.gold())
    embed.add_field(name="🔄 Servo Position", value=f"{current_status_servo} : {current_servo}º", inline=False)
    embed.add_field(name="💧 Pump", value=f"{current_pump}", inline=False)
    embed.add_field(name="💡 Lamp", value=f"{current_lamp}", inline=False)
    embed.add_field(name="🌡️ Temperature", value=f"{current_temp}º", inline=False)
    embed.add_field(name="🌙 AutoLamp", value="ON" if AUTO_LAMP else "OFF", inline=False)
    embed.add_field(name=f"⏳ {status}", value=f"⏳ {time_left}", inline=False)
    await channel.send(embed=embed)

# Embed Maker
@bot.command()
async def embeds(ctx, title :str , message :str):
    embed = discord.Embed(title=f"{title}", description=f"{message}", color=discord.Color.blue())
    await ctx.send(embed=embed)


# ========= ESP32 FORWARD INFORMATION TO DISCORD =========
def read_serial():
    global current_servo, current_lamp, current_pump, current_temp
    global Last_Alert, status_message

    if ser is None:
        print("Cannot do read_serial | Ser isn't connect")
        return

    while True:
        try:
            line = ser.readline().decode().strip()
            print("Data Dari ESP32:", line)

            parts = line.split("|")
            for part in parts:
                key, value = part.split(":")

                if key == "TEMP":
                    current_temp = float(value)
                elif key == "PUMP":
                    current_pump = value
                elif key == "LAMP":
                    current_lamp = value

                if status_message:
                    embed = discord.Embed(title="ESP32 LIVE DASHBOARD", color=discord.Color.green())
                    embed.add_field(name="🌡 Temperature", value=f"{current_temp}°C", inline=False)
                    embed.add_field(name="💧 Pump", value=current_pump, inline=False)
                    embed.add_field(name="💡 Lamp", value=current_lamp, inline=False)
                    embed.add_field(name="🔄 Servo", value=f"{current_servo}°", inline=False)
                    bot.loop.create_task(status_message.edit(embed=embed))
                
            if current_temp > MAX_TEMP:
                if time.time() - Last_Alert > Alert_Cooldown:
                    channel = bot.get_channel(CHANNEL_ID)
                    if channel:
                        embed = discord.Embed(title="⚠️ Warning High Temp", description=f"Suhu Mencapai {current_temp}º!")
                        notification = Notify()
                        notification.title("⚠️ Warning High Temp")
                        notification.message(f"Suhu Mencapai {current_temp} Gunakaan Kipas untuk mendingingkan")
                        notification.icon("/Users/bayu/Documents/IoT/icon.jpg")
                        
                        bot.loop.create_task(channel.send(embed=embed))
                        Last_Alert = time.time()
        except Exception as e:
            print("Error read_serial:", e)

threading.Thread(target=read_serial, daemon=True).start()

bot.run(TOKEN)