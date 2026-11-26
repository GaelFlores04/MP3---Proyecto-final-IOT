# Montaje y primera prueba del reproductor MP3

Los planos suponen un ESP32 DevKit clásico con los pines marcados como GPIO17, GPIO21, GPIO22 y GPIO27, un DFPlayer Mini y una OLED I²C de cuatro pines compatible con 3.3 V. Si tu placa dice ESP32-C3, ESP32-S3 o WROVER, o la pantalla tiene más de cuatro pines, verifica el modelo antes de seguir este plano.

- [Arquitectura del reproductor](docs/arquitectura.png)
- [Plano de conexiones](plano-conexiones.png)
- [Plano de la protoboard](plano-protoboard.png)
- [Guion de demostración y pruebas](docs/demo.md)

## 1. Prepara la tarjeta y la protoboard

1. Si la microSD ya tiene formato FAT32, úsala así. Si la vas a formatear, respalda primero los archivos. Crea carpetas numeradas `01`, `02`, etc. en la raíz y agrega un prefijo de tres cifras a cada MP3, empezando en `001` en cada álbum. Puedes conservar el título, por ejemplo `001 What Ever Happened.mp3`. Si tu variante del DFPlayer no abre nombres largos, usa `001.mp3` en la tarjeta y guarda el título en el controlador.
2. Conecta el ESP32 solo por USB. Si tienes multímetro, en voltaje continuo verifica que el pin marcado `5V` o `VIN` dé aproximadamente 5 V respecto a `GND`. Si no los da, verifica el modelo y su pin de 5 V antes de continuar. Desconecta el USB.
3. Monta el DFPlayer cruzando la ranura central. Si el ESP32 tapa los agujeros de ambos lados, colócalo en otra protoboard o usa cables hembra-macho. Deja acceso a las etiquetas y a la ranura de la microSD.

## 2. Conecta el DFPlayer y carga la prueba

Con el USB desconectado, arma estas conexiones. Deja la bocina y la OLED para después.

| ESP32 | DFPlayer | Cómo conectarlo |
| --- | --- | --- |
| 5V verificado | VCC | Riel positivo de 5 V |
| GND | GND | Riel negativo común |
| GPIO17 | RX | Resistencia de 1 kΩ en serie |
| GPIO27 | BUSY | Cable directo |

Los extremos de la resistencia de 1 kΩ deben quedar en dos grupos de agujeros eléctricamente separados. En un grupo llega GPIO17 y del otro sale un cable hacia RX. Deja libre TX del DFPlayer.

Inserta la microSD. Revisa que el riel de 5 V y GND estén separados y que las conexiones de VCC y GND coincidan con las etiquetas del módulo. Conecta USB y abre una terminal de ESP-IDF 6.1 en esta carpeta. Usa el puerto real de tu placa en lugar de `COMx`.

```powershell
idf.py set-target esp32
idf.py menuconfig
idf.py build
idf.py -p COMx flash monitor
```

En **Reproductor MP3 → Clave WPA2 de la red local**, escribe una clave propia de 8 a 63 caracteres. ESP-IDF la guarda en `sdkconfig`, excluido de Git. Conserva ese archivo y los binarios compilados en tu máquina. Sin clave válida, el ESP32 habilita el control BLE y deja apagado el punto de acceso. Para configurar sin menú, copia `sdkconfig.local.example` como `sdkconfig.local`, edita la copia y, en un directorio de compilación nuevo, ejecuta `idf.py -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.local" build`. Si ya existe un `sdkconfig`, ajusta la clave mediante el menú.

## 3. Conecta la bocina y verifica el audio

1. Desconecta el USB. Conecta un cable de la bocina a `SPK1` y el otro a `SPK2` del DFPlayer. Mantén ambas terminales de la bocina separadas de GND.
2. Conecta USB y, si el monitor se cerró al desconectar, vuelve a abrirlo con `idf.py -p COMx monitor`. El reproductor quedará **listo y en silencio**. Abre el controlador web y pulsa **Reproducir** para iniciar el audio a volumen **5/30**.
3. Si BUSY confirma reproducción pero no oyes sonido, revisa los dos cables SPK y su contacto con la bocina. Si BUSY no lo confirma, revisa la microSD, el archivo, GPIO17 → RX y GPIO27 → BUSY.

La bocina de 8 Ω y 1 W sirve para la prueba a volumen bajo. El valor 5/30 es un ajuste digital, no una medición de potencia. Evita usar el volumen máximo con la bocina conectada directamente. Si ya cuentas con una resistencia de potencia de **10 Ω y 2 W**, puedes intercalarla entre SPK1 y la bocina para reducir la potencia que recibe. La resistencia de **1 kΩ** pertenece a la señal RX.

## 4. Agrega la OLED

Apaga el montaje y conecta la pantalla por sus etiquetas.

| OLED | ESP32 |
| --- | --- |
| VCC | 3V3, por un cable separado del riel de 5 V |
| GND | GND común |
| SDA | GPIO21 |
| SCL | GPIO22 |

Energiza nuevamente y presiona **EN/RST**. La pantalla debe mostrar **PANTALLA OK** y después la pista en reproducción y el volumen. El monitor indica en qué dirección se inició la OLED (`0x3C` o `0x3D`).

## 5. Controla el reproductor desde el celular

1. Conecta el celular a la red **Reproductor-MP3** con la contraseña que configuraste antes de compilar. Si el celular avisa que la red no tiene internet, elige permanecer conectado.
2. Escribe **http://192.168.4.1** en la barra de direcciones del navegador del dispositivo conectado a **Reproductor-MP3**. Allí el ESP32 sirve la página y recibe los comandos. El archivo `control.html` de la carpeta del proyecto se puede abrir como vista previa.
3. Prueba **Pausar** y **Reanudar**, **Anterior** y **Siguiente**, y los botones de volumen. Comprueba que la OLED refleje la pista, el volumen y el estado.

Al encender se selecciona la primera pista de la carpeta `01` y se espera una orden de reproducción. Puedes seleccionar otra pista mientras está en espera. Cuando la lista termina, pulsa **Reproducir** para volver a la primera del álbum actual. El volumen va de **0 a 10** en la escala del DFPlayer, con nivel inicial **5**. Este límite es un ajuste de prueba para la bocina de 1 W y la potencia real depende de la señal.

### Elegir álbumes

En **Agregar o editar un álbum**, escribe el número de la carpeta en la microSD, el nombre que quieres ver en el celular y cuántos MP3 contiene. Por ejemplo, si la carpeta `01` contiene las once canciones de *Room On Fire*, registra carpeta **01**, nombre **Room On Fire** y **11** pistas. El número en la tarjeta indica el orden, con archivos continuos desde `001`.

Elige el álbum con el selector y pulsa **Reproducir**. Al cambiar de álbum, el ESP32 selecciona su primera pista y espera esa orden. Las letras, los títulos y las duraciones se guardan por álbum y pista. Si ya tenías datos de las primeras tres canciones de la carpeta `01`, el controlador sigue leyéndolos. La tarjeta del DFPlayer se prepara por separado.

La página y la OLED consultan el mismo reloj del ESP32. El cable `BUSY → GPIO27` le indica cuándo termina una pista. Si el módulo no confirma por ese cable, el reproductor pasa a la siguiente al cumplirse su duración guardada. En **Editar datos de una pista**, usa **Leer duración de un MP3** con la misma versión de la microSD o escribe su duración, por ejemplo `2:49`. Pulsa **Guardar en el ESP32**. El navegador lee la duración localmente y no envía el MP3. Un archivo LRC con `[length:02:49]` también puede llenar el campo.

## 6. Muestra el título, el artista y la letra sincronizada

La OLED muestra el título y el artista arriba. Cuando un nombre supera el ancho, el texto se desplaza horizontalmente. La letra aparece abajo.

1. Conserva los MP3 en las carpetas numeradas de la microSD. Prepara un archivo de texto **.lrc** en UTF-8 para cada pista, con una marca de tiempo al inicio de cada verso. Elige la misma versión de la canción para que coincidan los tiempos.
2. Selecciona el álbum en el controlador, abre **Editar datos de una pista**, selecciona su número y elige el archivo **.lrc** (máximo 4 KB). También puedes pegar el contenido directamente en la caja de texto.
3. Completa el título y el artista. Si el archivo incluye `[ti:...]` y `[ar:...]`, el controlador usa esos datos cuando los campos están vacíos. Pulsa **Guardar en el ESP32**. Los datos se conservan al apagarlo.

```text
[ti:Mi canción]
[ar:Mi artista]
[length:02:49]
[00:12.50] Primer verso de ejemplo
[00:18.00] Segundo verso de ejemplo
```

El ESP32 lee las marcas de tiempo y conserva la posición al pausar o reanudar. El cable `BUSY → GPIO27` ayuda a confirmar el inicio y el fin de cada pista. Si el módulo no confirma por `BUSY`, el ESP32 estima la posición a partir de la orden de reproducción. Para corregir un desfase uniforme, usa `[offset:+200]` para adelantar la letra 200 ms o `[offset:-200]` para retrasarla. La letra sin marcas se muestra con páginas manuales.

El repositorio incluye un [LRC de ejemplo](examples/ejemplo.lrc). Usa tus propios archivos MP3 para preparar la tarjeta.

[Referencia de conexiones y microSD del DFPlayer Mini](https://wiki.dfrobot.com/dfr0299)

## 7. Controla la microSD por Bluetooth

1. Abre **nRF Connect** o una app compatible con **Bluetooth LE UART** en el celular. Escanea desde la app.
2. Conéctate a **MP3-Control** y acepta el emparejamiento si aparece.
3. Dentro del servicio **Nordic UART** (`6E400001…`), escribe un comando como **texto UTF-8** en **RX** (`6E400002…`). Usa **Write Request**. Para ver cambios de estado, activa las notificaciones en **TX** (`6E400003…`).

| Comando | Acción |
| --- | --- |
| `reproducir` | Inicia o reanuda la pista |
| `pausar` | Pausa la reproducción |
| `anterior` / `siguiente` | Cambia de pista |
| `bajar` / `subir` | Ajusta el volumen un nivel |
| `album_02` | Selecciona el álbum registrado en la carpeta `02` y queda en espera |
| `pista_003` | Selecciona la pista 3 del álbum actual |

Un estado como `A02 P003 V05 PAUSA` indica álbum 02, pista 003, volumen 5 y reproducción pausada. El teléfono envía órdenes por Bluetooth y la música se reproduce desde la microSD por el DFPlayer. Para registrar álbumes o importar letras y duraciones, usa el controlador Wi-Fi.
