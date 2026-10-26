# OBS Projection Mapping

Gratis og enkel projection mapping direkte i OBS Studio (macOS først, Windows senere).

## Bruk

1. Lag en scene med innholdet du vil projisere (videoer, bilder, kamera …), f.eks. «Innhold».
2. Lag en ny scene, f.eks. «Projektor», og legg til kilden **Projection Mapping**.
3. I egenskapene: velg «Innhold» og sett projektorens oppløsning. Trykk OK.
4. Høyreklikk kilden → **Fullskjermprojektor (kilde)** → velg projektoren.
5. Høyreklikk kilden → **Samhandle** og dra hjørnene til de treffer flatene i rommet.
   - Dra midt i en flate for å flytte hele.
   - Modus «Velg utsnitt av innholdet» lar deg velge hvilken del av innholdet hver flate viser.
   - «+ Legg til flate» lager en ny flate.
6. Sett modus til «Ferdig» før visning, så forsvinner hjelpelinjene.

Mappingen lagres i OBS-scenesamlingen. Bytter du innhold i «Innhold»-scenen, beholdes mappingen.

## Bygging

GitHub Actions bygger en macOS-installasjonspakke (.pkg) ved hver push til `main`
(se fanen **Actions** → siste kjøring → **Artifacts**).

Basert på [obs-plugintemplate](https://github.com/obsproject/obs-plugintemplate). Lisens: GPL-2.0.
