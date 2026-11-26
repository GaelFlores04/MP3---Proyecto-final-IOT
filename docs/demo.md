# Demostración del reproductor MP3

## Preparación

- Alimentar el ESP32 por USB. Insertar la microSD y verificar que la OLED muestre el estado de espera.
- Registrar por Wi-Fi un álbum con por lo menos dos pistas y guardar la duración y el archivo LRC de ambas.
- Abrir nRF Connect en el iPhone. Tener a mano la contraseña local de la red del ESP32.

## Exposición de diez minutos

| Tiempo | Acción |
| --- | --- |
| 0:00–1:00 | Explicar cómo se reproduce música local y presentar el objetivo. |
| 1:00–2:00 | Mostrar el diagrama y seguir una orden del celular hasta el DFPlayer. |
| 2:00–6:00 | Hacer la demostración en vivo con el teléfono, la bocina y la OLED. |
| 6:00–8:00 | Presentar las pruebas confirmadas, los límites y el trabajo futuro. |
| 8:00–10:00 | Responder preguntas. |

## Secuencia de laboratorio

1. Mostrar que el equipo arranca en espera. Desde nRF Connect, conectar a MP3-Control y enviar `reproducir` como UTF-8 en RX. Verificar el audio y el cambio de estado en TX y OLED.
2. Enviar `pausar`, comprobar que se detiene el audio y reanudar. Enviar `siguiente` y confirmar el cambio de pista.
3. Abrir el controlador web y elegir un álbum. Reproducir una pista que tenga letra y mostrar dos líneas sucesivas en la OLED.
4. Verificar el cambio automático con una pista corta. Si el módulo no confirma BUSY, verificar la duración guardada de esa pista.

Para la grabación posterior, mostrar en un mismo encuadre el teléfono y la OLED, y conservar el audio de la bocina. Duración de 1 a 3 minutos. El video queda pendiente.
