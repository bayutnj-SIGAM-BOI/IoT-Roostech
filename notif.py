from notifypy import Notify

notification = Notify()
notification.title = "Cool Title"
notification.message = "Even cooler message."
notification.icon = "/Users/bayu/Documents/IoT/icon.jpg"

notification.send()
