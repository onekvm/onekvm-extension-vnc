module github.com/onekvm/onekvm-extension-vnc

go 1.25.0

require (
	github.com/amitbet/vnc2video v0.0.0-20190616012314-9d50b9dab1d9
	github.com/onekvm/onekvm-core v0.0.0
	github.com/rs/zerolog v1.33.0
)

require (
	github.com/mattn/go-colorable v0.1.13 // indirect
	github.com/mattn/go-isatty v0.0.20 // indirect
	golang.org/x/sys v0.47.0 // indirect
)

replace github.com/onekvm/onekvm-core => ../onekvm-core
