# Foldrion™: The Windows folder color utility
## Open Source, 100% free, minimalist, and secure.

![GitHub](https://img.shields.io/github/license/zonaro/Foldrion)
![GitHub Release Date](https://img.shields.io/github/release-date/zonaro/Foldrion)

</br>

![Foldrion Logo](readme_assets/before_after_header_trans_img_s.png)

### http://www.foldrion.com  

Coloring your Windows folders can help you organize and increase your productivity. Your folders are instantly recognizable by color vs having to pause to read their text label.  

Especially useful for programmers/developers, artists, power users, etc., that might have dozens of folders and multilevel nested folder trees to manage.  

### Latest release [download](https://github.com/zonaro/Foldrion/releases/)

## Installing
1) Download the Foldrion.exe executable.  
2) Run Foldrion.exe and click the "Install" button.
3) Optionally click the "Import Icon" button to copy `.ico` and `.dll` files, or convert `.jpg` / `.jpeg` / `.png` images into `.ico` files inside the program `icons` folder.

### The dialog window
![Foldrion Logo](readme_assets/ui_screenshot_install.png)  

</br>
Now installed, simply right click on a folder, select "Color Folder", and select one of the 14 color selections.

![Foldrion Logo](readme_assets/set_game_folder_white_anim.gif)

## Custom icon packs
Foldrion also supports custom icon resources from the installation folder.

1) Open the installation directory (normally under Program Files/Foldrion).
2) Add your custom files under an `icons` subfolder, or use the `Import Icon` button in the app.
3) Supported import formats are `.ico`, `.dll`, `.jpg`, `.jpeg`, and `.png`.
4) `.jpg`, `.jpeg`, and `.png` files are converted to `.ico` automatically during import.
4) Subfolders inside `icons` become nested context submenus automatically.
 

## Features
* Selection of 14 colors based on the pleasant Google Material Design palette.  
* 100% Free Open Source. You can known what it's made of and freely modify, remove, or add features to it.  
* Minimalist, systems-level designed. Consists of a single executable with an embedded icons resource.  
* Secure design: Absolute minimal API usage, using zero network calls. No adware, no nag screens, data collection, and doesn't use a vulnerable Explorer shell extension like others do.  

## Requirements  
Windows 7 to 11 32bit or 64bit OS.

## Motivation
After trying several commercial and free implementations of similar coloring tools, they were all lacking in one way or another.  Many had security red flags, advertising pop-ups, odd color schemes, using bloated Windows explorer shell Extensions, and/or with disagreeable license terms.  
I could see there really wasn't that much too them functionally so it was time to roll my own, make the better simpler one that I desired and then share with others.

See http://www.foldrion.com for more details.

## Bugs, Issues, Feature requests
Please report any potential bugs or other issues, suggestions, feature requests, using the github issue feautre.    
https://github.com/zonaro/Foldrion/issues  

## Building
Developed in C/C++ using [Microsoft Visual Studio 2022](https://visualstudio.microsoft.com):  

Python 3.10 using the [PyCharm IDE](https://www.jetbrains.com/pycharm ) for procedurally generating the color icon sets.   

See http://www.foldrion.com for more development details.  

## Contributing
If you'd like to contribute, please fork the repository and use a feature
branch. Pull requests are welcomed.

## Licensing
The code and assets in this project is licensed under MIT license.  
Copyright (c) 2026 Zonaro 
https://github.com/zonaro/Foldrion/blob/main/LICENSE

