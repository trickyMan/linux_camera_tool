# Leopard USB3.0 Camera Linux Camera Tool

  This is the sample code for Leopard USB3.0 camera linux camera tool. It is a simple interface for capturing and viewing video from v4l2 devices, with a special emphasis for the linux uvc driver. 
  
  Author: Danyu L
  Last edit: 2019/04

## Installation
### Dependencies
Make sure the libraries have installed. Run confugure.sh for completing installing all the required dependencies
```sh
chmod +X configure.sh
./configure.sh
```

### OpenCV Prequisites
Make sure you have GTK 3 and OpenCV (3 at least) installed. The way you do to install this package varies by operational system.

Gtk3 asnd Gtk2 don't live together paceful. If you try to run camera tool and got this error message:

    Gtk-ERROR **: GTK+ 2.x symbols detected. Using GTK+ 2.x and GTK+ 3 in the same proc

It is mostly you have OpenCV build with GTk2 support. The only way to fix it is rebuild OpenCV without GTk2:

    opencv_dir/release$cmake [other options] -D WITH_GTK_2_X=OFF ..

in order to disable Gtk2 from OpenCV.

```sh
#rebuild opencv, this might take a while
rm -rf build/
mkdir build && cd build/
cmake -D CMAKE_BUILD_TYPE=RELEASE -D CMAKE_INSTALL_PREFIX=/usr/local -D WITH_TBB=ON -D BUILD_NEW_PYTHON_EXAMPLES=ON -D BUILD_EXAMPLES=ON -D WITH_QT=ON -D WITH_OPENGL=ON -D WITH_GTK=ON -D WITH_GTK3=ON -D WITH_GTK_2_X=OFF ..
make -j6            #do a $lscpu to see how many cores for CPU
sudo make install -j6

#link opencv
sudo sh -c 'echo "/usr/local/lib" >> /etc/ld.so.conf.d/opencv.conf'
sudo ldconfig
```

### Build Camera Tool(Makefile)
```sh
make
```

### Build Camera Tool(CMake)
```sh
mkdir build
cd build
cmake ../
make
```

### Add to your project(CMake)
```sh
add_subdirectory(linux_camera_tool)
include_directories(
	linux_camera_tool/src/
)

...

target_link_libraries(<Your Target>
	leopard_tools
)
```


### Run Camera Tool
```
./leopard_cam
```
### Examples
__Original streaming for IMX477__ -> image is dark and blue
<img src="pic/477orig.jpg" width="1000">
__Modified streaming for IMX477:__
1. enabled software AWB
2. increase exposure
3. read & write register from IMX477

<img src="pic/477regCtrl.jpg" width="1000">

__Modified streaming for OS05A20 full resolution__
1. change bayer pattern to GRBG
2. enable software AWB
3. increase exposure, gain

<img src="pic/os05a20.jpg" width="1000">

__Modified OS05A20 resolution to an available one__
1. binning mode
2. capture raw and bmp, save them in the camera tool directory
<img src="pic/changeResOS05A20.jpg" width="1000">




__Original streaming for AR1335 ICP3 YUV cam:__
Default ae enabled -> change exposure&gain takes no effects
<img src="pic/aeEnableNotChangeExp.jpg" width="1000">

__Disable ae:__
Being able to change exposure & gain taking effective
<img src="pic/aeDisable.jpg" width="1000">


### Exit Camera Tool
Use __ESC__ on both of the gui.

### Kill Camera Tool Windows Left Over
If you forget to exit both windows and tried to run the camera tool again, it will give you the error of 
```sh
VIDIOC_DQBUF: Invalid argument
```
Please check your available processes and kill leopard_cam, then restart the camera tool application
```sh
ps
killall -9 leopard_cam
```

## Test Platform
- __4.18.0-17-generic #18~18.04.1-Ubuntu SMP__ 
- __4.15.0-32-generic #35~16.04.1-Ubuntu__
- __4.15.0-20-generic #21~Ubuntu 18.04.1 LTS__


## Known Bugs
### Exposure & Gain Control Momentarily Split Screen
When changing exposure and gain under linux, camera tool will experience split screen issue at the moment change is happened. This issue happens for the USB3 camera that use manual DMA in the FX3 driver. For the camera that utilizes auto DMA, the image will be ok when exposure and gain change happens. 

For updating driver from manual DMA to auto DMA, you need to ensure:
1. PTS embedded information is not needed in each frame
2. The camera board has a Crosslink FPGA on the tester board that is able to add uvc header to quality for auto DMA
3. FX3 driver also need to be updated.

### SerDes Camera Experiences First Frame Bad When Uses Trigger
When use triggering mode instead of master free running mode for USB3 SerDes camera, the very first frame received will be bad and should be tossed away. It is recommended to use an external function generator or a dedicated triggering signal for triggering the cameras for the purpose of syncing different SerDes cameras. 

The included "shot 1 trigger" function is only a demonstration on generating one pulse and let camera output one frame after "shot 1 trigger" is clicked once. User should not fully rely on this software generated trigger but use a hardware trigger for sync the camera streaming.

### Detect Two Video Devices
__FIXME:__ 
After add udev, this thing happen, the workaround now is to exit the udev list loop once we get one video device that is from Leopard.



