Conebeam CT reconstruction for CT data from the nanoSPECT scanner.

Code is a bit of a mess, with the reconstruction work in ct_recon_win.h and the GUI in win_cone_ct.cpp.
Multithreaded so the GUI doesn't hang while the reconstruction is running. Beam hardening correction is hardcoded, so it's calibrated for the scanner I was working with at the time.

Any questions should be directed to jared.strydhorst@gmail.com
