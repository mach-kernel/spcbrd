.FIRST: play

WATCOM=https://github.com/open-watcom/open-watcom-v2/releases/download/Current-build/open-watcom-2_0-c-dos.exe

init:
	mkdir -p dosbox/c

watcom: init
	curl -Lo dosbox/c/setup.exe $(WATCOM)
	dosbox-x -conf dosbox/dosbox-setup-watcom.conf

dev:
	dosbox-x -conf dosbox/dosbox.conf

play:
	dosbox-x -conf dosbox/dosbox-play.conf