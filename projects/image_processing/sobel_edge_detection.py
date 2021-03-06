#!/usr/bin/env python
import cv
from cv2 import *
import math

# Set the camera
cam = VideoCapture(0)

# Read an image and save it
s, image = cam.read()
if s:
	# *** USER: change name of the file
	imwrite('cam_image.jpg', image)

# Open the image 
image = cv.LoadImage('cam_image.jpg',cv.CV_LOAD_IMAGE_GRAYSCALE)

# Code from glowing python (glowingpython.blogspot.co.uk/2011/10/beginning-with-opencv-in-python.html)
# Create a matrix of the same size than the original but with one channel.
dstSobel = cv.CreateMat(image.height, image.width, cv.CV_32FC1)

# Apply sobel algorithm
cv.Sobel(image, dstSobel, 1, 1, 3)
# End of code from glowing python	
	
# Save the image
cv.SaveImage('sobel.jpg', dstSobel)
