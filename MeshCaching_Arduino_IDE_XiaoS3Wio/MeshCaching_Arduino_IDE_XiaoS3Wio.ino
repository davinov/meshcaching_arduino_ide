/**
 * @file    MeshCaching_Arduino_IDE_XiaoS3Wio.ino
 * @brief   Géolocalisation d'un répéteur Meshcore
 *
 * @details Mesure le RSSI (niveau de signal) et le temps écoulé depuis la
 *          dernière réception d'un paquet provenant d'un répéteur Meshcore
 *          spécifique, sur un Seeed XIAO ESP32S3 + Wio-SX1262 (le kit
 *          vendu pour Meshtastic/MeshCore, sans écran).
 *
 *          Faute d'écran, les résultats s'affichent sur le smartphone, en
 *          Bluetooth (service "Nordic UART") ou par câble USB :
 *            - interface web https://david.nowinsky.net/meshcaching-ui/
 *              (RSSI en grand, graphique, carte, ping) ;
 *            - ou simples lignes de texte dans une appli terminal
 *              ("Serial Bluetooth Terminal", "Serial USB Terminal").
 *
 * @see     https://tutoduino.fr/
 * @see     https://david.nowinsky.net/meshcaching-ui/
 */

#include <SPI.h>
#include <RadioLib.h>

// =====================================================================
// 0) SORTIE BLUETOOTH (1 = active, 0 = inactive ; le port USB reste toujours actif)
// =====================================================================
#define USE_BLE_UART 1  // Bluetooth Low Energy, service Nordic UART

#if USE_BLE_UART
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLE2902.h>
#endif

// =====================================================================
// 1) BROCHAGE — XIAO ESP32S3 + Wio-SX1262 (connecteur carte a carte du kit)
//    Valeurs identiques aux variantes officielles Meshtastic (seeed_xiao_s3)
//    et MeshCore (xiao_s3_wio).
// =====================================================================
#define LORA_SCK_PIN 7
#define LORA_MISO_PIN 8
#define LORA_MOSI_PIN 9
#define LORA_CS_PIN 41
#define LORA_RST_PIN 42
#define LORA_DIO1_PIN 39
#define LORA_BUSY_PIN 40
#define LORA_RXEN_PIN 38  // commutateur d'antenne (reception), l'emission est pilotee par DIO2

#define BUTTON_PIN 21       // bouton utilisateur de la carte Wio-SX1262
#define BOOT_BUTTON_PIN 0   // bouton "B" (BOOT) du XIAO, utilisable aussi pour le ping
#define BUTTON_COOLDOWN_MS 5000UL  // delai mini entre deux pings (duty cycle)

// =====================================================================
// 2) PARAMETRES RADIO — Meshcore Ile de France
// =====================================================================
#define LORA_FREQ_MHZ 869.618
#define LORA_BW_KHZ 62.5
#define LORA_SF 8
#define LORA_CR 8
#define LORA_TX_POWER 14           // le SX1262 monte a 22 dBm, mais 14 dBm suffit et respecte la reglementation EU
#define LORA_PREAMBLE_LEN 16       // preambule utilise par MeshCore
#define LORA_TCXO_VOLTAGE 1.8      // le Wio-SX1262 a un TCXO alimente par DIO3 en 1.8 V
#define LORA_CURRENT_LIMIT 140     // mA, comme le firmware MeshCore pour cette carte

// !! A ADAPTER : prefixe de la cle publique du repeteur MeshCore vise.
//const uint8_t TARGET_PUBKEY_PREFIX[] = { 0xC6, 0xF1 };
//const uint8_t TARGET_PUBKEY_PREFIX[] = { 0xE0, 0xB6 };
// Chasse du 27/09/2026 : "MeshCaching-IdF-2026", cle 57dbbfb74dd235672d91d041b32114dc8719979ebfb7c0d80be82f03c38e7489
const uint8_t TARGET_PUBKEY_PREFIX[] = { 0x57, 0xDB };
#define TARGET_PUBKEY_PREFIX_LEN (sizeof(TARGET_PUBKEY_PREFIX))

// Detection des paquets FLOOD retransmis par le repeteur (identifies par le
// dernier hash du chemin, d'un seul octet en general).
//   1 = actif  : mises a jour frequentes, mais un AUTRE repeteur dont la cle
//                publique commence par le meme octet peut etre confondu
//                avec la cible (1 chance sur 256).
//   0 = inactif : on ne retient que les ANNONCES du repeteur (cle publique
//                complete, aucune ambiguite) et les reponses a notre ping.
//                Detection plus rare, mais sans aucune fausse alerte.
#define ACCEPT_RELAYED_FLOOD 1

// =====================================================================
// 3) FORMAT DE PAQUET MESHCORE (doc officielle meshcore-dev/MeshCore)
//    header (1 octet) = VV PPPP RR  (version / payload type / route type)
// =====================================================================
#define ROUTE_TYPE_TRANSPORT_FLOOD 0x00
#define ROUTE_TYPE_FLOOD 0x01
#define ROUTE_TYPE_DIRECT 0x02
#define ROUTE_TYPE_TRANSPORT_DIRECT 0x03

#define PAYLOAD_TYPE_ADVERT 0x04
#define PAYLOAD_TYPE_TRACE 0x09  // "trace un chemin" : c'est le ping natif de MeshCore

#define MAX_PACKET_LEN 256
#define ADVERT_PUBKEY_LEN 32  // une annonce contient la cle publique complete (32 octets)

// =====================================================================
// 4) ETAT GLOBAL DE L'APPLICATION
// =====================================================================
// On regroupe tout ce qui concerne "le dernier paquet vu du repeteur cible"
// dans une seule structure : c'est plus simple à lire qu'une liste de
// variables globales éparpillées.
struct RepeaterStatus {
  bool hasPacket = false;  // avons-nous deja recu un paquet du repeteur cible ?
  unsigned long lastSeenMs = 0;
  float rssi = 0;
  float snr = 0;
  const char *why = "";     // raison de la derniere detection
  uint32_t detections = 0;  // nombre de paquets du repeteur cible (sert a reperer les nouveaux)
  uint32_t packetsHeard = 0;  // tous les paquets valides recus, cible ou non
};

RepeaterStatus targetStatus;

// Tag de la derniere requete TRACE envoyee, et instant d'envoi : permet de
// reconnaitre la reponse (qui renvoie ce meme tag) sans avoir a en decoder
// un hash, puisque le format du payload TRACE est different des messages.
uint32_t lastSentTag = 0;
unsigned long lastPingMs = 0;
bool hasPinged = false;
#define TRACE_REPLY_TIMEOUT_MS 10000UL  // on n'accepte une reponse que dans les 10s suivant le ping

// Nom de l'appareil Bluetooth : "MeshCaching-E0B6"
char deviceName[24];

SX1262 lora = new Module(LORA_CS_PIN, LORA_DIO1_PIN, LORA_RST_PIN, LORA_BUSY_PIN);

// Ce drapeau est mis a "true" par l'interruption radio des qu'un paquet arrive.
// On le traite ensuite tranquillement dans loop(), jamais dans l'interruption
// elle-meme (regle de base avec RadioLib / ESP32).
volatile bool packetReceived = false;
void IRAM_ATTR onPacketReceivedISR() {
  packetReceived = true;
}

// =====================================================================
// 5) BLUETOOTH : SERVICE NORDIC UART
// =====================================================================
// Le service "Nordic UART" (NUS) est un port serie sans fil standard :
// n'importe quelle appli "terminal Bluetooth" sait l'afficher. On y envoie
// une ligne par paquet du repeteur, et on accepte ces commandes (aussi
// depuis un terminal sur le port USB, voir loop()) :
//   "p" (ou "ping")   : envoie un ping TRACE
//   "s" (ou "status") : renvoie l'etat courant
//   "j"               : renvoie l'etat en JSON sur une ligne, pour l'interface
//                       web meshcaching-ui (repondu seulement a celui qui demande)

// Les callbacks BLE tournent dans une autre tache que loop() : on se contente
// de lever des drapeaux, traites ensuite dans loop() (comme pour la radio).
volatile bool pingRequested = false;
volatile bool statusRequested = false;
volatile bool bleJsonRequested = false;
bool serialJsonRequested = false;

#if USE_BLE_UART
#define NUS_SERVICE_UUID "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define NUS_RX_UUID "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"  // telephone -> XIAO
#define NUS_TX_UUID "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"  // XIAO -> telephone

BLECharacteristic *bleTx = nullptr;
volatile bool bleConnected = false;

class BleServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer *server) override {
    bleConnected = true;
    statusRequested = true;  // on accueille le telephone avec l'etat courant
  }
  void onDisconnect(BLEServer *server) override {
    bleConnected = false;
    server->startAdvertising();  // redevient visible pour une reconnexion
  }
};

class BleRxCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *c) override {
    size_t len = c->getLength();
    if (len == 0) return;
    char cmd = tolower(c->getData()[0]);
    if (cmd == 'p') pingRequested = true;
    if (cmd == 's') statusRequested = true;
    if (cmd == 'j') bleJsonRequested = true;
  }
};

void initBle() {
  BLEDevice::init(deviceName);
  BLEServer *server = BLEDevice::createServer();
  server->setCallbacks(new BleServerCallbacks());

  BLEService *service = server->createService(NUS_SERVICE_UUID);
  bleTx = service->createCharacteristic(NUS_TX_UUID, BLECharacteristic::PROPERTY_NOTIFY);
#if !defined(CONFIG_NIMBLE_ENABLED)
  // Avec Bluedroid, il faut ajouter le descripteur 2902 pour que le telephone
  // puisse s'abonner aux notifications (NimBLE l'ajoute tout seul).
  bleTx->addDescriptor(new BLE2902());
#endif
  BLECharacteristic *rx = service->createCharacteristic(
    NUS_RX_UUID, BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR);
  rx->setCallbacks(new BleRxCallbacks());
  service->start();

  BLEAdvertising *adv = BLEDevice::getAdvertising();
  adv->addServiceUUID(NUS_SERVICE_UUID);
  adv->setScanResponse(true);
  BLEDevice::startAdvertising();
  Serial.printf("Bluetooth actif : %s\n", deviceName);
}

// Envoie une ligne au telephone, decoupee en morceaux de 20 octets : c'est
// la taille garantie d'une notification BLE, quelle que soit l'appli.
void bleSendLine(const char *line) {
  if (!bleConnected || bleTx == nullptr) return;
  size_t len = strlen(line);
  for (size_t i = 0; i < len; i += 20) {
    size_t chunk = min((size_t)20, len - i);
    bleTx->setValue((uint8_t *)line + i, chunk);
    bleTx->notify();
    delay(5);
  }
}
#endif

// =====================================================================
// 6) SORTIE TEXTE (moniteur serie + Bluetooth)
// =====================================================================
// Meme texte pour le moniteur serie et le Bluetooth : printf "maison".
void report(const char *fmt, ...) {
  char line[160];
  va_list args;
  va_start(args, fmt);
  vsnprintf(line, sizeof(line), fmt, args);
  va_end(args);
  Serial.print(line);
#if USE_BLE_UART
  bleSendLine(line);
#endif
}

// Etat courant en JSON (commande "j")
void formatStatusJson(char *json, size_t size) {
  unsigned long age = targetStatus.hasPacket ? (millis() - targetStatus.lastSeenMs) / 1000 : 0;
  snprintf(json, size,
           "{\"target\":\"%02X%02X\",\"has\":%s,\"rssi\":%.1f,\"snr\":%.1f,"
           "\"age\":%lu,\"why\":\"%s\",\"seq\":%lu,\"rx\":%lu,\"pingWait\":%lu}",
           TARGET_PUBKEY_PREFIX[0], TARGET_PUBKEY_PREFIX[1],
           targetStatus.hasPacket ? "true" : "false",
           targetStatus.rssi, targetStatus.snr, age, targetStatus.why,
           (unsigned long)targetStatus.detections, (unsigned long)targetStatus.packetsHeard,
           pingWaitSeconds());
}

void reportStatus() {
  if (!targetStatus.hasPacket) {
    report("%s : en attente de paquets (%lu entendus)\n", deviceName,
           (unsigned long)targetStatus.packetsHeard);
    return;
  }
  unsigned long secondesEcoulees = (millis() - targetStatus.lastSeenMs) / 1000;
  report("%02X%02X : %.0f dBm, SNR %.1f dB, il y a %lus\n",
         TARGET_PUBKEY_PREFIX[0], TARGET_PUBKEY_PREFIX[1],
         targetStatus.rssi, targetStatus.snr, secondesEcoulees);
}

// =====================================================================
// 7) INITIALISATION RADIO
// =====================================================================
void initRadio() {
  Serial.println(F("Initialisation LoRa..."));
  SPI.begin(LORA_SCK_PIN, LORA_MISO_PIN, LORA_MOSI_PIN, LORA_CS_PIN);

  // Commutateur d'antenne : RXEN (GPIO38) pour la reception, DIO2 du SX1262
  // pour l'emission. Sans ca, la radio "fonctionne" mais n'entend presque rien.
  lora.setRfSwitchPins(LORA_RXEN_PIN, RADIOLIB_NC);

  int state = lora.begin(LORA_FREQ_MHZ, LORA_BW_KHZ, LORA_SF, LORA_CR,
                         RADIOLIB_SX126X_SYNC_WORD_PRIVATE, LORA_TX_POWER,
                         LORA_PREAMBLE_LEN, LORA_TCXO_VOLTAGE);
  if (state != RADIOLIB_ERR_NONE) {
    // Le code d'erreur est repete chaque seconde : on le voit meme si le
    // moniteur serie est ouvert apres le demarrage.
    while (true) {
      Serial.printf("Erreur LoRa : %d  (-2 = radio non detectee : carte Wio-SX1262 mal enfichee ?)\n", state);
      delay(1000);
    }
  }

  lora.setDio2AsRfSwitch(true);
  lora.setCurrentLimit(LORA_CURRENT_LIMIT);
  lora.setRxBoostedGainMode(true);  // meilleure sensibilite, utile pour les signaux faibles
  lora.setDio1Action(onPacketReceivedISR);

  state = lora.startReceive();
  if (state != RADIOLIB_ERR_NONE) {
    Serial.printf("Erreur startReceive : %d\n", state);
  }
  Serial.println(F("LoRa OK"));
}

// =====================================================================
// 8) ENVOI D'UN "PING" (paquet TRACE) VERS LE REPETEUR CIBLE
// =====================================================================
// MeshCore a une fonctionnalite native pour ca : le payload TRACE (0x09),
// concu pour tracer un chemin et collecter le SNR de chaque saut. Avec un
// seul noeud dans la liste, c'est l'equivalent MeshCore d'un ping.
// Format confirme par la doc officielle (wiki MeshCore, "Companion Radio
// Protocol") : [tag 4o][auth_code 4o][flags 1o][liste de hash a tracer].
//
// Ici on vise un seul repeteur, en "zero-hop" (path_len=0 au niveau du
// paquet) : le paquet est emis une seule fois, sans etre relaye plus loin.
void sendTracePing() {
  uint8_t buf[16];
  size_t offset = 0;

  // --- Header du paquet : payload TRACE + route DIRECT ---
  buf[offset++] = (PAYLOAD_TYPE_TRACE << 2) | ROUTE_TYPE_DIRECT;

  // --- path_length du paquet (niveau routage) : 0 = zero-hop ---
  buf[offset++] = 0x00;
  // (pas d'octets de chemin a la suite puisqu'il n'y a aucun saut)

  // --- Payload TRACE ---
  uint32_t tag = esp_random();  // identifiant aleatoire de cette requete
  memcpy(buf + offset, &tag, sizeof(tag));
  offset += sizeof(tag);

  lastSentTag = tag;  // on retiendra ce tag pour reconnaitre la reponse
  lastPingMs = millis();
  hasPinged = true;

  uint32_t authCode = 0;  // pas de code d'authentification particulier
  memcpy(buf + offset, &authCode, sizeof(authCode));
  offset += sizeof(authCode);

  buf[offset++] = 0x00;  // flags, reserve pour l'instant

  // Liste des noeuds a tracer : un seul, le repeteur cible
  buf[offset++] = TARGET_PUBKEY_PREFIX[0];

  report("Envoi TRACE (tag=%08lX) vers REPETEUR %02X...\n",
         (unsigned long)tag, TARGET_PUBKEY_PREFIX[0]);

  int state = lora.transmit(buf, offset);
  if (state != RADIOLIB_ERR_NONE) {
    report("Erreur d'emission : %d\n", state);
  }

  // transmit() declenche aussi une interruption DIO1 de "fin d'emission",
  // qui peut avoir arme packetReceived a tort : on l'ignore avant de
  // repasser en ecoute, pour ne pas traiter du vide comme un paquet recu.
  packetReceived = false;
  lora.startReceive();
}

// Nombre de secondes a attendre avant le prochain ping autorise (0 = maintenant)
unsigned long pingWaitSeconds() {
  if (!hasPinged) return 0;
  unsigned long elapsed = millis() - lastPingMs;
  if (elapsed >= BUTTON_COOLDOWN_MS) return 0;
  return (BUTTON_COOLDOWN_MS - elapsed + 999) / 1000;
}

// Point d'entree unique pour les pings (boutons, Bluetooth, USB), avec le
// delai minimal entre deux emissions. Renvoie true si le ping est parti.
bool requestPing() {
  if (pingWaitSeconds() > 0) return false;
  sendTracePing();
  return true;
}

// =====================================================================
// 9) DECODAGE DU PAQUET MESHCORE
// =====================================================================
// Determine si le paquet a ete EMIS par le repeteur cible : c'est SON signal
// dont on veut afficher le RSSI. On ne retient donc un paquet que si son
// emetteur est identifiable avec certitude. Il n'y a que trois cas :
//
//  1. Reponse a notre ping TRACE : reconnue par son tag aleatoire.
//  2. ANNONCE (advert) sans saut : le payload commence par la cle publique
//     complete de l'emetteur, qui est forcement celui qui vient d'emettre.
//  3. Paquet FLOOD retransmis (au moins un saut) : chaque repeteur ajoute son
//     hash a la fin du chemin avant de retransmettre, donc le DERNIER hash est
//     le dernier repeteur qui a emis le paquet. (Voir ACCEPT_RELAYED_FLOOD.)
//
// Tout le reste est REFUSE, car l'emetteur n'est pas identifiable :
//  - Paquets en route DIRECT : le chemin contient les sauts qu'il RESTE a
//    faire (path[0] = prochain saut, dernier = destination finale), pas
//    l'emetteur. Un paquet destine "via" le repeteur ne vient pas de lui.
//  - Premier octet du payload : ce n'est PAS le hash de l'emetteur. Pour
//    REQ / RESPONSE / TXT_MSG / PATH c'est le hash du DESTINATAIRE (l'emetteur
//    vient ensuite), pour GRP_TXT le hash du canal, pour ACK un bout de CRC.
//    Un message ADRESSE au repeteur (ex. requete d'un smartphone) etait donc
//    pris a tort pour un paquet emis par lui.
//  - Messages de groupe, ACK, etc. : l'emetteur n'y figure pas du tout.
//
// Rappel du format d'un paquet MeshCore :
//   [header 1 octet] [transport_codes 0 ou 4 octets] [path_length 1 octet]
//   [path 0-64 octets] [payload 0-184 octets]
//
// "why" (optionnel) recoit la raison de la detection, pour le moniteur serie.
bool packetComesFromTarget(const uint8_t *packet, size_t len, const char **why) {
  if (len < 2) return false;

  uint8_t header = packet[0];
  uint8_t routeType = header & 0x03;
  uint8_t payloadType = (header >> 2) & 0x0F;

  size_t offset = 1;

  // Les modes "transport" ajoutent 4 octets de code de transport
  if (routeType == ROUTE_TYPE_TRANSPORT_FLOOD || routeType == ROUTE_TYPE_TRANSPORT_DIRECT) {
    offset += 4;
  }
  if (offset >= len) return false;

  // Octet path_length : bits 0-5 = nombre de sauts, bits 6-7 = (taille du hash - 1)
  uint8_t pathLengthByte = packet[offset];
  uint8_t hopCount = pathLengthByte & 0x3F;
  uint8_t hashSize = ((pathLengthByte >> 6) & 0x03) + 1;
  offset += 1;

  const uint8_t *pathBytes = packet + offset;
  offset += (size_t)hopCount * hashSize;
  if (offset > len) return false;  // paquet incoherent, on l'ignore

  const uint8_t *payload = packet + offset;
  size_t payloadLen = len - offset;

  // --- Cas 1 : reponse a notre TRACE ---
  // Un paquet TRACE ne contient PAS de hash en tete de payload : il commence
  // par un tag aleatoire de 4 octets. On compare son tag a celui de notre
  // dernier ping envoye.
  if (payloadType == PAYLOAD_TYPE_TRACE) {
    if (payloadLen < 4 || lastSentTag == 0) return false;
    if (millis() - lastPingMs > TRACE_REPLY_TIMEOUT_MS) return false;

    uint32_t receivedTag;
    memcpy(&receivedTag, payload, sizeof(receivedTag));
    if (receivedTag != lastSentTag) return false;
    if (why) *why = "reponse TRACE";
    return true;
  }

  // --- Cas 2 : annonce sans saut (peu importe la route : une annonce "zero-hop"
  //     est emise avec un chemin vide, et personne ne l'a retransmise) ---
  if (payloadType == PAYLOAD_TYPE_ADVERT && hopCount == 0) {
    if (payloadLen < ADVERT_PUBKEY_LEN) return false;
    // Comparaison sur TOUT le prefixe configure (cle complete cote paquet)
    if (memcmp(payload, TARGET_PUBKEY_PREFIX, TARGET_PUBKEY_PREFIX_LEN) != 0) return false;
    if (why) *why = "annonce";
    return true;
  }

  // --- Cas 3 : paquet FLOOD retransmis, dernier hash du chemin = dernier emetteur ---
#if ACCEPT_RELAYED_FLOOD
  bool isFlood = (routeType == ROUTE_TYPE_FLOOD || routeType == ROUTE_TYPE_TRANSPORT_FLOOD);
  if (isFlood && hopCount >= 1) {
    const uint8_t *lastHopId = pathBytes + (size_t)(hopCount - 1) * hashSize;
    size_t compareLen = min((size_t)hashSize, (size_t)TARGET_PUBKEY_PREFIX_LEN);
    if (memcmp(lastHopId, TARGET_PUBKEY_PREFIX, compareLen) != 0) return false;
    if (why) *why = "relais flood";
    return true;
  }
#endif

  // Tout le reste : emetteur non identifiable, on ne retient pas.
  return false;
}

// =====================================================================
// 10) TRAITEMENT D'UN PAQUET RECU
// =====================================================================
void handleIncomingPacket() {
  uint8_t buf[MAX_PACKET_LEN];
  size_t len = lora.getPacketLength();
  if (len == 0 || len > sizeof(buf)) {
    lora.startReceive();
    return;
  }

  int state = lora.readData(buf, len);
  float rssi = lora.getRSSI();
  float snr = lora.getSNR();

  // On ignore les paquets illisibles OU dont le CRC est invalide :
  // un paquet corrompu ne doit jamais etre interprete comme venant du
  // repeteur cible (risque de fausse detection).
  if (state != RADIOLIB_ERR_NONE) {
    if (state != RADIOLIB_ERR_CRC_MISMATCH) {
      Serial.printf("Erreur de reception : %d\n", state);
    }
    lora.startReceive();
    return;
  }

  targetStatus.packetsHeard++;

  const char *why = "";
  bool isTarget = packetComesFromTarget(buf, len, &why);

  if (isTarget) {
    targetStatus.hasPacket = true;
    targetStatus.lastSeenMs = millis();
    targetStatus.rssi = rssi;
    targetStatus.snr = snr;
    targetStatus.why = why;
    targetStatus.detections++;

    // Ligne courte, pensee pour l'ecran d'un telephone (Bluetooth)
    report("CIBLE %02X%02X : %.0f dBm, SNR %.1f dB (%s)\n",
           TARGET_PUBKEY_PREFIX[0], TARGET_PUBKEY_PREFIX[1], rssi, snr, why);
  } else {
    // Paquets des autres noeuds : moniteur serie uniquement, pour ne pas
    // noyer le terminal Bluetooth.
    Serial.printf("Paquet recu : len=%u RSSI=%.1f dBm SNR=%.1f dB (ignore)\n",
                  (unsigned)len, rssi, snr);
  }

  lora.startReceive();
}

// =====================================================================
// 11) SETUP / LOOP
// =====================================================================
void setup() {
  Serial.begin(115200);
  delay(200);

  // Boutons relies a la masse quand ils sont presses
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  pinMode(BOOT_BUTTON_PIN, INPUT_PULLUP);

  snprintf(deviceName, sizeof(deviceName), "MeshCaching-%02X%02X",
           TARGET_PUBKEY_PREFIX[0], TARGET_PUBKEY_PREFIX[1]);

  initRadio();

#if USE_BLE_UART
  initBle();
#endif

  Serial.println(F("En attente de paquets MeshCore..."));
}

void loop() {
  // Commandes tapees dans un terminal USB : seule la premiere lettre de
  // chaque ligne compte, comme en Bluetooth ("p", "ping", "s", "status").
  static bool atLineStart = true;
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\r' || c == '\n') {
      atLineStart = true;
      continue;
    }
    if (atLineStart) {
      c = tolower(c);
      if (c == 'p') pingRequested = true;
      if (c == 's') statusRequested = true;
      if (c == 'j') serialJsonRequested = true;
    }
    atLineStart = false;
  }

  // Reponse JSON, uniquement sur le lien qui l'a demandee : l'interface web
  // interroge chaque seconde, inutile d'en inonder l'autre terminal.
  if (serialJsonRequested || bleJsonRequested) {
    char json[256];
    formatStatusJson(json, sizeof(json));
    strcat(json, "\n");
    if (serialJsonRequested) Serial.print(json);
#if USE_BLE_UART
    if (bleJsonRequested) bleSendLine(json);
#endif
    serialJsonRequested = bleJsonRequested = false;
  }

  // Rappel periodique de l'etat au telephone, pour suivre le "temps ecoule".
  // "Serial" est vrai quand un terminal a ouvert le port USB.
  bool phoneListening = Serial;
#if USE_BLE_UART
  phoneListening = phoneListening || bleConnected;
#endif
  static unsigned long lastStatus = 0;
  if (phoneListening && millis() - lastStatus >= 10000) {
    statusRequested = true;
  }
  if (statusRequested) {
    statusRequested = false;
    lastStatus = millis();
    reportStatus();
  }
  if (pingRequested) {
    pingRequested = false;
    if (!requestPing()) {
      report("Ping trop tot : reessayez dans %lu s\n", pingWaitSeconds());
    }
  }

  // Appui sur un bouton : on force un ping TRACE vers le repeteur cible,
  // au lieu d'attendre passivement son prochain paquet. requestPing()
  // impose 5 secondes entre deux emissions (duty cycle).
  if (digitalRead(BUTTON_PIN) == LOW || digitalRead(BOOT_BUTTON_PIN) == LOW) {
    requestPing();
  }

  if (packetReceived) {
    packetReceived = false;
    handleIncomingPacket();
  }
}
