# program: track editor

Thhe solution contains a number of race tracks in folder content/tracks/real
each track has a .yaml file with data such as metadata, centerline spline and raceline. we also have the .msgpack files which contain a messagepack-serialized mesh of the track.

This program allows to visualize this data and edit it.

The program is written in Rust and uses Bevy engine for rendering.

STEP 1 - Program startup. It shows a splash screen while everythihng is being loaded.

STEP 2. The next screen shows a short description of the program and a browse button. this should select a folder with tracks.

STEP 3. In the current screen, populate a list of tracks that can be opened for editing - both a yaml file and a msgpack file should exist.

STEP 4. Allow the user to select a track and click a button with text "Edit"

STEP 5. Once a track is opened, the main window shows the track in 3D view, and the user can use AWSD and the mouse to move the camera around the track. For this the track mesh should be rendered.

STEP 6. to be determined