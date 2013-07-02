#   PhoneUtils - Author: Tom Richardson, Radu Jipa 2013
#   For use with PiMote and pimoteutils


from pimoteutils import *


########################-------SERVER-------########################################
#  This is the main server that runs on the pi. All messages are sorted here and sent
#  to the phone that handles them.
#  It also initialises and sorts the security.

class PhoneServer(PiMoteServer):
  phone = None

  #Store the phone object for reference
  def addPhone(self, thephone): 
    self.phone = thephone

  def messageReceived(self, message):
    if isinstance(self.phone, Phone): #Regular phone
      (id, sep, msg) = message.strip().partition(",") #Strip component ID and message apart
      self.phone.updateButtons(int(id), msg) #Update buttons if needed
      self.phone.buttonPressed(int(id), msg) #Allow the user to handle the message
    elif isinstance(self.phone, ControllerPhone): #Controller
      self.phone.controlPress(message) #Controller handler

################------PHONE TYPES--------####################

class Phone():
  buttons = [] #For user use
  outputs = []
  components = [] #To send to phone

  controltype = 0 #Type of phone

  video = False

  #More final protocol variables for setup
  INPUT_REGULAR = 1
  INPUT_TEXT = 2
  INPUT_TOGGLE = 3
  OUTPUT_TEXT = 4
  VIDEO_FEED = 5
  VOICE_INPUT = 6
  #Setup
  SET_CONTROL_TYPE = 0
  SETUP = 1
  #Data being sent
  REQUEST_OUTPUT_CHANGE = 2

  #Add a button to the phone
  def addButton(self, button):
    if isinstance(button, Button):
      button.id = len(self.components)
      self.components.append(button)
    else:
      print("Button not provided")

  #Add an output to the phone
  def addOutput(self, output):
    if isinstance(output, OutputText):
      output.id = len(self.components)
      self.components.append(output)
    else:
      print("Not an output")

  #Add a video feed to the phone
  def addVideoFeed(self, vid):
    if self.video:
      print("You can only have one video feed running..")
      sys.exit(0)
    else:
      self.vid = vid
      self.video = True
      self.components.append(vid)

  #User overrides this. Called when a message is recieved
  def buttonPressed(self, id, msg):
    pass
  #Used for setup
  def setup(self, socket):
    self.socket = socket
    socket.send(str(Phone.SET_CONTROL_TYPE)+","+str(self.controltype))
    for c in self.components:
      c.setup(socket) #setup each component
  #Updates the state of buttons (toggle)
  def updateButtons(self, id, message):
    for b in self.buttons:
      if b.id == id:
        if isinstance(b, ToggleButton):
          value = False
          if(int(message) == 1):
            value = True
          b.setValue(value)
  


class ControllerPhone():
  controltype = 1
  video = False
  voice = False
  def controlPress(self, type):
    pass
  def setVideo(self, value):
    self.video = value
  def setVoice(self, value):
    self.voice = value
  def setup(self, socket):
    voiceV = videoV = 0
    if self.video == True:
      videoV = 1
    if self.voice == True:
      voiceV = 1
    socket.send(str(Phone.SET_CONTROL_TYPE)+","+str(self.controltype) + "," + str(videoV) + "," + str(voiceV))



####################----COMPONENTS----######################

class Button():
  def __init__(self, name):
    self.name = name
    self.type = Phone.INPUT_REGULAR
  def getId(self):
    return self.id
  def getName(self):
    return self.name
  def getType(self):
    return self.type
  def setup(self, socket):
    socket.send(str(PiMoteServer.MESSAGE_FOR_MANAGER)+","+str(Phone.SETUP)+","+str(self.type) + "," + str(self.id) + "," + str(self.name))

class InputText(Button):
  def __init__(self, name):
    self.name = name
    self.type = Phone.INPUT_TEXT
  def setup(self, socket):
    socket.send(str(PiMoteServer.MESSAGE_FOR_MANAGER)+","+str(Phone.SETUP)+","+str(self.type) + "," + str(self.id) + "," + str(self.name))

class ToggleButton(Button):
  def __init__(self, name, initialvalue):
    self.name = name
    self.value = initialvalue
    self.type = Phone.INPUT_TOGGLE
  def getValue(self):
    return self.value
  def setValue(self, value):
    self.value = value
  def setup(self, socket):
    tf = 0
    if self.value == True:
      tf=1
    socket.send(str(PiMoteServer.MESSAGE_FOR_MANAGER)+","+str(Phone.SETUP)+","+str(self.type) + "," + str(self.id) + "," + str(self.name) + "," + str(tf))

class VoiceInput(Button):
  def __init__(self):
    self.type = Phone.VOICE_INPUT
  def setup(self, socket):
    socket.send(str(PiMoteServer.MESSAGE_FOR_MANAGER)+","+str(Phone.SETUP)+","+str(self.type)+","+str(self.id))



class OutputText():
  message = ""
  def __init__(self, initialmessage):
    self.type = Phone.OUTPUT_TEXT
    self.message = initialmessage
  def setText(self, message):
    self.message = message
    self.socket.send(str(PiMoteServer.MESSAGE_FOR_MANAGER)+","+str(Phone.REQUEST_OUTPUT_CHANGE)+","+str(self.id)+","+str(self.message))
  def getText(self):
    return self.message
  def setup(self, socket):
    self.socket = socket
    socket.send(str(PiMoteServer.MESSAGE_FOR_MANAGER)+","+str(Phone.SETUP)+","+str(self.type)+","+str(self.id)+","+str(self.message)) 

class VideoFeed():
  outsidefeed = 0;
  ip = "-"
  def __init__(self, width, height):
    self.type = Phone.VIDEO_FEED
    self.width = width
    self.height = height
  def setIp(self, ip):
    self.ip = ip
    self.outsidefeed = 1
  def setup(self, socket):
    socket.send(str(PiMoteServer.MESSAGE_FOR_MANAGER)+","+str(Phone.SETUP)+","+str(self.type)+","+str(self.width)+","+str(self.height)+","+str(self.outsidefeed)+","+self.ip)
