# CRYENGINE
This repository houses the source code for CRYENGINE.
Instructions on getting started with Git can be found [here](https://docs.cryengine.com/display/CEPROG/Getting+Started+with+git), along with details on working with projects created via the CRYENGINE Launcher and Git source code.
   
   
## Building
CRYENGINE uses the CMake project generation and build tools. For more information please visit the [CMake Documentation](https://docs.cryengine.com/display/CEPROG/CMake).
In order to compile, you will need to install:
Visual Studio 2017, 2019 or 2022 with
* Desktop Development with C++ Package
* Optional Windows SDK 10.0.20348.0
* Optional C++ MFC for your build tools (Required by Sandbox)
* [FBX SDK (2016.1)](https://images.autodesk.com/adsk/files/fbx20161_fbxsdk_vs2015_win0.exe) (Required by Sandbox)
   
   
Steps:
1. Run `cry_cmake.exe` in the repository folder root. This will download and extract the SDK zip we provide to the correct location.
    **Note:** If for any reason the automatic SDK Zip download does not work, please follow the additional steps mentioned below:
    1. Download the SDKs zip from the [releases page](https://github.com/CRYTEK/CRYENGINE_Source/releases).
    2. Extract the SDK zip contents to `Code/SDKs`.
2. _If you want to build the Sandbox Editor:_
    1. Download and Install the FBX SDK to the default location `C:\Program Files\Autodesk\FBX\FBX SDK\2016.1`.
    2. Copy `C:\Program Files\Autodesk\FBX\FBX SDK\2016.1` to `Code/SDKs` and rename it to `FbxSdk`.
    3. You may uninstall the FBX SDK from the default location in step 2.
3. Select the options in the CMake GUI dialog and click `Configure`. (Some options may require proprietary SDKs that we cannot provide and which you must license).
4. Verify the options and then click `Generate`.
5. If you would like to associate this Engine with the project\'s [right-click context menu](https://docs.cryengine.com/display/CEMANUAL/Project+Tools) in Windows File Explorer:
    1. Run the file `Tools/CryVersionSelector/install.bat`.
6. If you would like to register this Engine to appear in the "Switch Engine version" [project context menu options](https://docs.cryengine.com/display/CEMANUAL/Project+Tools):
    1. Open the `cryengine.cryengine` file in the repository folder root with a plain text file editor (eg, Notepad).
    2. Modify the line `"id": "engine-5.7",` to something unique. For example: `"id": "engine-5.7-git1",`.
    3. Modify the line `"name": "CRYENGINE 5.7",` to something memorable. For example: `"name": "CRYENGINE 5.7 - GitHub No.1",`
    
**Note:** The generated Visual Studio solution file will be located at `solutions_cmake/win64/CRYENGINE_Win64.sln`
   
## Branches
- The `Release` branch provides stable snapshots of the Engine. Most of the commits to this branch are [tagged](https://github.com/CRYTEK/CRYENGINE/releases).
- The `Stabilisation` branch is used for fixing bugs in the run-up to release. This branch is created by branching from `main` or the previous release, depending on if it's a major (feature) or minor (stability) release respectively.
- A `Release_Candidate` branch may be created for a few days before a release, if we need to make critical fixes before the actual release.
Development takes place primarily in the `Main` branch. This branch is not currently available for the general public.
   
   
## Pull Requests
Pull requests are currently on hold, while we fix all the workflow issues the current process had. For more details, see this [announcement post](https://www.cryengine.com/news/cryengine-on-github-live-updates-to-main-will-go-on-hiatus). We apologize for the inconvenience.
   
   
## Licenses
The source code in this repository is governed by the CRYENGINE license agreement, which is contained in [LICENSE.md](LICENSE.md), adjacent to this README.md file. See also the FAQ [here](FAQ.md).
```diff
+ Please note that as of March 20th 2018, the new CRYENGINE business model is in effect.
+ That means 5% royalties apply to projects developed and published on CRYENGINE 5.0 and beyond.
+ Check our new FAQ.md for all facts and exemptions.
```
CRYENGINE uses certain third-party software/code which is governed by respective third-party licenses. You may find such third-party licenses in the distribution under `/Tools/thirdparty_licenses`, in addition, you may find a list of all third-party software/code, the licenses and further information with regard to the usage of third-party software/code in CRYENGINE in the [online Documentation](https://docs.cryengine.com/display/RN/Third+Party+SDKs+in+5.7) and/or in the online [FAQ.](FAQ.md) Please carefully review the information from the online documentation and the online FAQ.# SmileEngine
