# GraphWar Clone

C++17 + SFML 3.1 prototype using the user's working static MSVC build.

## Current features

- LAN host/client over TCP port 42069.
- Host-authoritative projectile simulation.
- Random player positions.
- Random circular destructible obstacles.
- Obstacles have HP and are destroyed without changing radius.
- Configurable field width/height.
- Configurable graph scale and turn time.
- Optional trajectory prediction.
- Fullscreen with aspect-correct world scaling.
- Mathematical expressions with x, a, b, c, pi and e.
- Coefficient values for a, b and c.
- Separate vertical constant C.
- Function history stores both expression and coefficient values.
- Reconnection retries and explicit session disconnect handling for repeated LAN matches.

## Function examples

```text
a*x^2+b*x+c
sin(x)*a+b
c*sin(a*x)+b
2*sin(x)+a*x+b
```

## Controls

### Match

- `TAB` - edit function
- `ENTER` - apply function
- `BACKSPACE` - delete
- `UP/DOWN` - power; while editing, browse function history
- `LEFT/RIGHT` - constant C
- `Q/A` - coefficient a +/-
- `W/S` - coefficient b +/-
- `E/D` - coefficient c +/-
- `F1..F4` - preset functions
- `F5/F6` - browse function history
- `SPACE` - fire
- `F11` - fullscreen

### Build

The project is intended for the working MSVC task configuration:

```text
cl /EHsc /std:c++17 /MD /D SFML_STATIC /I C:/Libraries/SFML/include main.cpp Expression.cpp /link /LIBPATH:C:/Libraries/SFML/lib sfml-graphics-s.lib sfml-window-s.lib sfml-system-s.lib sfml-network-s.lib opengl32.lib winmm.lib gdi32.lib freetype.lib harfbuzz.lib ws2_32.lib user32.lib ole32.lib dnsapi.lib advapi32.lib mbedtls.lib mbedx509.lib mbedcrypto.lib bcrypt.lib crypt32.lib /OUT:app.exe
```
