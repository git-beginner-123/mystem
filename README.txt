Build:
- idf.py set-target esp32s3
- idf.py build flash monitor

Display ST7735:
SCK=GPIO21 MOSI=GPIO47 CS=GPIO41 DC=GPIO40 RST=GPIO45 BLK=GPIO42

Input UART1 frame:
GPIO35/36, frame AA CMD 55
CMD 01..05, CMD05 treated as Enter
