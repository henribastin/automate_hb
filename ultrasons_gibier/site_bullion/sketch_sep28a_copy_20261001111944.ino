#include <ESP8266WiFi.h>
#include <WiFiClientSecure.h>
#include <UniversalTelegramBot.h>
#include <ArduinoJson.h>
#include <EEPROM.h>
#include <Wire.h>
#include "SSD1306Ascii.h"
#include "SSD1306AsciiWire.h"

// =========================================================================
// CONFIGURATIONS CONNEXION (À COMPLÉTER)
// ==== PA à intégrer ==================================================
// --- WI-FI & TELEGRAM ---
const char* ssid = "xxxxxxxxxx";
const char* password = "xxxxxxxxxxx";

#define BOTtoken "xxxxxxxxxxxx" 
#define CHAT_ID "xxxxxxxxxx"

//==== FIN PA =========================================================
//
// CE BLOC RE-DÉCLARE LA VARIABLE 'bot' POUR TOUT LE PROGRAMME :
WiFiClientSecure client;
UniversalTelegramBot bot(BOTtoken, client);
SSD1306AsciiWire oled;

// BROCHAGE DÉFINITIF VALIDÉ
//=====================================================================
const int pinRelaisOption = D0;     // Commande Relais Option indépendant (Bouton poussoir)
const int pinIN1 = D1;              // Sortie 1 vers Pont en H L298N (Ultrasons) [^1]
const int pinIN2 = D2;              // Sortie 2 vers Pont en H L298N (Ultrasons) [^1]
const int pinControlePuissance = 4; // Assigné sur G4 (GPIO4) : Signal PWM vers Régulateur DC-DC
const int pinSCL = D5;              // Écran OLED SCL
const int pinSDA = D6;              // Écran OLED SDA
const int pinSignalBarriere = D7;   // Entrée Signal Optocoupleur Barrière Infrarouge [^1]
const int pinRelaisStrobo = D8;     // Commande stable Relais Stroboscope (0V en veille)

// =========================================================================
// PARAMÈTRES DE CONFIGURATION & ALERTE
// =========================================================================
long dureeTotaleAlerte = 60;   // Durée initiale de l'alerte (Paramétrable Telegram, Paliers de 60s)
long dureeRallongeAlerte = 15; // Temps additionnel si détection pendant alerte (en secondes)
int frequenceCentrale = 20000; // Fréquence de base (20 kHz)
String modeActuel = "SWEEP";   // Mode balayage de fréquence par défaut

// Paramètres Stroboscope
int nbRepetitionsStrobo = 20;  // Nombre de flashs par défaut
int dureeFlashMs = 80;         // Cadence (Allumé/Éteint) en millisecondes

// Paramètres de Puissance Matérielle (G4)
int niveauPuissance = 3;       // 1 = Faible (30m), 2 = Moyen (60m), 3 = Maximum (100m)
int valeurPwmPuissance = 1023; // 1023 = Plein pot 12V (Sans régulateur raccordé = 12V par défaut) [^1]

// Variables de gestion de temps (Salves & Veille)
unsigned long tempsDebutPauseSalve = 0;
unsigned long dureePauseAleatoire = 0;
bool enPhasePauseSalve = false;
bool enCoursEmission = false;
unsigned long tempsDeclenchement = 0;
unsigned long dernierScanTelegram = 0;
unsigned long dernierMessageTempsRestant = 0; 
bool ancienEtatBarriere = HIGH;
wl_status_t ancienStatutWifi = WL_DISCONNECTED;

// Adresses EEPROM
const int ADDR_DESACTIVE = 0;
const int ADDR_BIP = 1;
const int ADDR_DUREE = 2; 
const int ADDR_STROBO = 3; 
const int ADDR_OPTION = 4; 

bool systemeDesactiveManuel = false; 
bool bipAudibleActif = true;         
bool stroboActif = true;       
bool relaisOptionEtat = false;  

// =========================================================================
// GESTION MÉMOIRE & ÉCRAN OLED
// =========================================================================
void chargerParametres() {
  EEPROM.begin(8);
  systemeDesactiveManuel = (EEPROM.read(ADDR_DESACTIVE) == 1);
  bipAudibleActif = (EEPROM.read(ADDR_BIP) == 1);
  stroboActif = (EEPROM.read(ADDR_STROBO) == 1);
  relaisOptionEtat = (EEPROM.read(ADDR_OPTION) == 1);
  
  long dureeStockee = EEPROM.read(ADDR_DUREE);
  if (dureeStockee > 0) {
    dureeTotaleAlerte = dureeStockee * 4; 
  }
}

void sauvegarderParametres() {
  EEPROM.write(ADDR_DESACTIVE, systemeDesactiveManuel ? 1 : 0);
  EEPROM.write(ADDR_BIP, bipAudibleActif ? 1 : 0);
  EEPROM.write(ADDR_STROBO, stroboActif ? 1 : 0);
  EEPROM.write(ADDR_OPTION, relaisOptionEtat ? 1 : 0);
  EEPROM.write(ADDR_DUREE, (byte)(dureeTotaleAlerte / 4)); 
  EEPROM.commit();
  mettreAJourOled();
}

void mettreAJourOled() {
  oled.clear();
  oled.setCursor(0,0);
  oled.println("ALIM 12V : OK");
  
  bool wifiOk = (WiFi.status() == WL_CONNECTED);
  if (wifiOk) { oled.print("WI-FI : "); oled.println(ssid); } 
  else { oled.println("WI-FI : RECHERCHE..."); }
  oled.println("---------------------");
  
  if (enCoursEmission) { oled.println(wifiOk ? "ALERTE : EN COURS !" : "DECLENCHEMENT (ANALOG)"); } 
  else { oled.println("ALERTE : VEILLE"); }
  
  oled.print("BIP:"); oled.print(bipAudibleActif ? "ON " : "OFF ");
  oled.print("STRB:"); oled.println(stroboActif ? "ON" : "OFF");
  
  oled.print("POUV: "); 
  if (niveauPuissance == 1) oled.println("FAIBLE");
  else if (niveauPuissance == 2) oled.println("MOYEN");
  else oled.println("MAXIMUM");
  
  oled.print("ULTRASONS : "); oled.println(systemeDesactiveManuel ? "OFF (COUPE)" : "ON (ARME)");
}

// =========================================================================
// SEQUENCES CYCLES MATÉRIELS (STROBO & ULTRASONS)
// =========================================================================
void executerFlashsStrobo() {
  if (!stroboActif) return;
  for (int i = 0; i < nbRepetitionsStrobo; i++) {
    digitalWrite(pinRelaisStrobo, HIGH); delay(dureeFlashMs);                 
    digitalWrite(pinRelaisStrobo, LOW);  delay(dureeFlashMs);                 
  }
}

void allumerUltrasons(String source) {
  enCoursEmission = true;
  tempsDeclenchement = millis();
  dernierMessageTempsRestant = millis(); 
  enPhasePauseSalve = false;
  mettreAJourOled();
  
  if (WiFi.status() == WL_CONNECTED) {
    String txt = "🚨 **Déclenchement** : " + source + " (" + String(dureeTotaleAlerte) + "s)";
    String clavier = "[[{\"text\":\"🛑 Arrêter l'Alarme\",\"callback_data\":\"/stop_urgence\"}]]";
    bot.sendMessageWithInlineKeyboard(CHAT_ID, txt, "Markdown", clavier);
  }

  if (bipAudibleActif) {
    analogWriteFreq(3000); analogWrite(pinIN1, 512); digitalWrite(pinIN2, LOW);
    delay(300);            analogWrite(pinIN1, 0);   digitalWrite(pinIN2, LOW);
  }

  if (stroboActif) { executerFlashsStrobo(); }
  mettreAJourOled();
}

void ajouterRallongeAlerte() {
  tempsDeclenchement += (dureeRallongeAlerte * 1000); 
  
  if (WiFi.status() == WL_CONNECTED) {
    String txt = "🔄 **Nouvelle présence détectée !** +" + String(dureeRallongeAlerte) + "s ajoutées.";
    String clavier = "[[{\"text\":\"🛑 Arrêter l'Alarme\",\"callback_data\":\"/stop_urgence\"}]]";
    bot.sendMessageWithInlineKeyboard(CHAT_ID, txt, "Markdown", clavier);
  }
  
  if (stroboActif) { executerFlashsStrobo(); } 
  enPhasePauseSalve = false; 
  mettreAJourOled();
}

void eteindreUltrasons() {
  enCoursEmission = false;
  analogWrite(pinIN1, 0);
  analogWrite(pinIN2, 0);
  digitalWrite(pinRelaisStrobo, LOW); 
  mettreAJourOled();
}

// =========================================================================
// INTERFACES ET SOUS-MENUS GRAPHIQUES TELEGRAM (TEXTES NETTOYÉS)
// =========================================================================
void envoyerMenuPrincipal() {
  if (WiFi.status() != WL_CONNECTED) return; 
  String txt = "🎛️ **Tableau de Bord Principal**\n\n";
  txt += "📡 **SURVEILLANCE :** " + String(systemeDesactiveManuel ? "`❌ COUPÉE`" : "`✅ EN COURS`") + "\n";
  txt += "🔊 **ALERTE BIP :** " + String(bipAudibleActif ? "`🔊 ACTIF`" : "`🤫 COUPE`") + "\n";
  txt += "⚡ **STROBOSCOPE :** " + String(stroboActif ? "`🟢 ON`" : "`⚫ OFF`") + "\n";
  txt += "⚙️ **OPTION D0 :** " + String(relaisOptionEtat ? "`🟩 ON`" : "`🟥 OFF`") + "\n";

  String clavier = "[";
  clavier += "[{\"text\":\"📡 Réglages Surveillance\",\"callback_data\":\"/m_surv\"}],";
  clavier += "[{\"text\":\"🔊 Réglages Alerte Bip\",\"callback_data\":\"/m_bip\"}],";
  clavier += "[{\"text\":\"⚡ Réglages Stroboscope\",\"callback_data\":\"/m_strobo\"}],";
  clavier += "[{\"text\":\"⚙️ Réglages Option D0\",\"callback_data\":\"/m_opt\"}]";
  clavier += "]";
  bot.sendMessageWithInlineKeyboard(CHAT_ID, txt, "Markdown", clavier);
}

void envoyerMenuSurveillance() {
  String txt = "📡 **Paramètres de Surveillance & Alarme**\n\n";
  txt += "• État : " + String(systemeDesactiveManuel ? "❌ Coupée" : "🛡️ Armée") + "\n";
  txt += "• Durée totale alerte : `" + String(dureeTotaleAlerte) + "s` (Max 600s)\n";
  txt += "• Rallonge par ré-impulsion : `" + String(dureeRallongeAlerte) + "s`\n";
  String pStr = (niveauPuissance == 1) ? "FAIBLE" : (niveauPuissance == 2) ? "MOYEN" : "MAXIMUM";
  txt += "• Puissance G4 : *" + pStr + "*";
  
  String clavier = "[";
  clavier += "[{\"text\":\"Armer\",\"callback_data\":\"/activer\"},";
  clavier += "{\"text\":\"Couper\",\"callback_data\":\"/couper\"}],";
  clavier += "[{\"text\":\"Alerte -60s\",\"callback_data\":\"/moins60\"},";
  clavier += "{\"text\":\"Alerte +60s\",\"callback_data\":\"/plus60\"}],";
  clavier += "[{\"text\":\"Rallonge -5s\",\"callback_data\":\"/ral_moins\"},";
  clavier += "{\"text\":\"Rallonge +5s\",\"callback_data\":\"/ral_plus\"}],";
  clavier += "[{\"text\":\"Faible\",\"callback_data\":\"/p_faible\"},";
  clavier += "{\"text\":\"Moyen\",\"callback_data\":\"/p_moyen\"},";
  clavier += "{\"text\":\"Maximum\",\"callback_data\":\"/p_max\"}],";
  clavier += "[{\"text\":\"Test Alarme Totale\",\"callback_data\":\"/test\"}],";
  clavier += "[{\"text\":\"Retour Menu\",\"callback_data\":\"/menu\"}]";
  clavier += "]";
  bot.sendMessageWithInlineKeyboard(CHAT_ID, txt, "Markdown", clavier);
}

void envoyerMenuBip() {
  String txt = "🔊 **Paramètres du Bip Humain Audible**\n\n";
  txt += "• État : " + String(bipAudibleActif ? "🔊 ACTIVÉ" : "🤫 MUTÉ");
  
  String clavier = "[";
  clavier += "[{\"text\":\"🔊 Activer Bip\",\"callback_data\":\"/bip_on\"},";
  clavier += "{\"text\":\"🤫 Muter Bip\",\"callback_data\":\"/bip_off\"}],";
  clavier += "[{\"text\":\"🎯 Tester Bip Seul\",\"callback_data\":\"/test_bip\"}],";
  clavier += "[{\"text\":\"↩️ Retour Menu\",\"callback_data\":\"/menu\"}]";
  clavier += "]";
  bot.sendMessageWithInlineKeyboard(CHAT_ID, txt, "Markdown", clavier);
}

void envoyerMenuStrobo() {
  String txt = "⚡ **Paramètres du Stroboscope**\n\n";
  txt += "• État : " + String(stroboActif ? "🟢 ACTIVÉ" : "⚫ DÉSACTIVÉ") + "\n";
  txt += "• Nombre de flashs : `" + String(nbRepetitionsStrobo) + "`\n";
  txt += "• Cadence (allumé/éteint) : `" + String(dureeFlashMs) + " ms`";
  
  String clavier = "[";
  clavier += "[{\"text\":\"⚡ Strobo ON\",\"callback_data\":\"/strobo_on\"},";
  clavier += "{\"text\":\"⚫ Strobo OFF\",\"callback_data\":\"/strobo_off\"}],";
  clavier += "[{\"text\":\"💥 Flashs -5\",\"callback_data\":\"/flash_moins\"},";
  clavier += "{\"text\":\"💥 Flashs +5\",\"callback_data\":\"/flash_plus\"}],";
  clavier += "[{\"text\":\"⏱ Cadence -20ms\",\"callback_data\":\"/cadence_moins\"},";
  clavier += "{\"text\":\"⏱ Cadence +20ms\",\"callback_data\":\"/cadence_plus\"}],";
  clavier += "[{\"text\":\"🎯 Tester Strobo Seul\",\"callback_data\":\"/test_strobo\"}],";
  clavier += "[{\"text\":\"↩️ Retour Menu\",\"callback_data\":\"/menu\"}]";
  clavier += "]";
  bot.sendMessageWithInlineKeyboard(CHAT_ID, txt, "Markdown", clavier);
}

void envoyerMenuOption() {
  String txt = "⚙️ **Paramètres de l'Option (Relais D0)**\n\n";
  txt += "• État permanent : " + String(relaisOptionEtat ? "🟩 ALLUMÉ (ON)" : "🟥 ÉTEINT (OFF)") + "\n";
  txt += "_(L'action impulsionnelle transmet un contact de 500ms inconditionnel)_";
  
  String clavier = "[";
  clavier += "[{\"text\":\"🟢 Forcer ON\",\"callback_data\":\"/opt_on\"},";
  clavier += "{\"text\":\"🔴 Forcer OFF\",\"callback_data\":\"/opt_off\"}],";
  clavier += "[{\"text\":\"⚡ Action Impulsion (500ms)\",\"callback_data\":\"/opt_pulse\"}],";
  clavier += "[{\"text\":\"↩️ Retour Menu\",\"callback_data\":\"/menu\"}]";
  clavier += "]";
  bot.sendMessageWithInlineKeyboard(CHAT_ID, txt, "Markdown", clavier);
}

void gererNouveauxMessages(int numNewMessages) {
  for (int i = 0; i < numNewMessages; i++) {
    String chat_id = String(bot.messages[i].chat_id);
    if (chat_id != CHAT_ID) continue; 

    // Décodeur standard : Votre version de la librairie met le clic du bouton directement dans .text
    String commande = bot.messages[i].text;
    
    // Routage vers le Menu Principal ou les Sous-Menus
    if (commande == "/menu" || commande == "/start" || commande == "/status") { envoyerMenuPrincipal(); }
    else if (commande == "/m_surv") { envoyerMenuSurveillance(); }
    else if (commande == "/m_bip") { envoyerMenuBip(); }
    else if (commande == "/m_strobo") { envoyerMenuStrobo(); }
    else if (commande == "/m_opt") { envoyerMenuOption(); }
    
    // Arrêt d'urgence direct de l'alarme
    else if (commande == "/stop_urgence") {
      eteindreUltrasons();
      bot.sendMessage(CHAT_ID, "🛑 **Alarme interrompue manuellement.**", "Markdown");
      envoyerMenuPrincipal();
    }
    
    // Commandes du sous-menu Surveillance
    else if (commande == "/test") { allumerUltrasons("TEST GLOBAL MANUEL"); }
    else if (commande == "/couper") { systemeDesactiveManuel = true; sauvegarderParametres(); envoyerMenuSurveillance(); }
    else if (commande == "/activer") { systemeDesactiveManuel = false; sauvegarderParametres(); envoyerMenuSurveillance(); }
    else if (commande == "/plus60") { if (dureeTotaleAlerte <= 540) { dureeTotaleAlerte += 60; sauvegarderParametres(); } envoyerMenuSurveillance(); }
    else if (commande == "/moins60") { if (dureeTotaleAlerte >= 120) { dureeTotaleAlerte -= 60; sauvegarderParametres(); } envoyerMenuSurveillance(); }
    else if (commande == "/ral_plus") { if (dureeRallongeAlerte <= 55) { dureeRallongeAlerte += 5; } envoyerMenuSurveillance(); }
    else if (commande == "/ral_moins") { if (dureeRallongeAlerte >= 5) { dureeRallongeAlerte -= 5; } envoyerMenuSurveillance(); }
    
    // Commandes du sous-menu Puissance (G4)
    else if (commande == "/p_faible") { niveauPuissance = 1; valeurPwmPuissance = 400; analogWrite(pinControlePuissance, valeurPwmPuissance); envoyerMenuSurveillance(); }
    else if (commande == "/p_moyen") { niveauPuissance = 2; valeurPwmPuissance = 750; analogWrite(pinControlePuissance, valeurPwmPuissance); envoyerMenuSurveillance(); }
    else if (commande == "/p_max") { niveauPuissance = 3; valeurPwmPuissance = 1023; analogWrite(pinControlePuissance, valeurPwmPuissance); envoyerMenuSurveillance(); }

    // Commandes du sous-menu Alerte Bip
    else if (commande == "/bip_on") { bipAudibleActif = true; sauvegarderParametres(); envoyerMenuBip(); }
    else if (commande == "/bip_off") { bipAudibleActif = false; sauvegarderParametres(); envoyerMenuBip(); }
    else if (commande == "/test_bip") {
      // Notification avant de lancer le son
      bot.sendMessage(CHAT_ID, "🔊 **Exécution du test Bip Seul (500ms)...**", "Markdown");
      
      analogWriteFreq(3000); analogWrite(pinIN1, 512); digitalWrite(pinIN2, LOW); delay(500);
      analogWrite(pinIN1, 0); analogWrite(pinIN2, 0);
      
      // AJOUT : Message de confirmation après le bip
      bot.sendMessage(CHAT_ID, "✅ **Test Bip terminé.**", "Markdown");
    }
    
    // Commandes du sous-menu Option Relais D0
    else if (commande == "/opt_on") { relaisOptionEtat = true; sauvegarderParametres(); digitalWrite(pinRelaisOption, HIGH); envoyerMenuOption(); }
    else if (commande == "/opt_off") { relaisOptionEtat = false; sauvegarderParametres(); digitalWrite(pinRelaisOption, LOW); envoyerMenuOption(); }
    else if (commande == "/opt_pulse") {
      // AJOUT : Vérification de l'état. Si l'état permanent est sur OFF, l'impulsion est impossible.
      if (relaisOptionEtat == false) {
        bot.sendMessage(CHAT_ID, "❌ **Action impossible** : Le relais D0 est désactivé (OFF). Veuillez l'activer (ON) avant de lancer une impulsion.", "Markdown");
      } 
      else {
        // Envoi du message spécifique de type "Déclenchement" avec bouton d'arrêt intégré
        if (WiFi.status() == WL_CONNECTED) {
          String txt = "🚨 **Déclenchement** : ACTION IMPULSION RELAIS D0";
          String clavier = "[[{\"text\":\"🛑 Arrêter l'Alarme\",\"callback_data\":\"/stop_urgence\"}]]";
          bot.sendMessageWithInlineKeyboard(CHAT_ID, txt, "Markdown", clavier);
        }
        
        // Exécution physique de l'impulsion de 500ms sur la broche D0
        digitalWrite(pinRelaisOption, HIGH); 
        delay(500);
        digitalWrite(pinRelaisOption, relaisOptionEtat ? HIGH : LOW); 
      }
    }
    // Commandes du sous-menu Stroboscope
    else if (commande == "/strobo_on") { stroboActif = true; sauvegarderParametres(); envoyerMenuStrobo(); }
    else if (commande == "/strobo_off") { stroboActif = false; sauvegarderParametres(); envoyerMenuStrobo(); }
    else if (commande == "/flash_plus") { if (nbRepetitionsStrobo <= 95) { nbRepetitionsStrobo += 5; } envoyerMenuStrobo(); }
    else if (commande == "/flash_moins") { if (nbRepetitionsStrobo >= 10) { nbRepetitionsStrobo -= 5; } envoyerMenuStrobo(); }
    else if (commande == "/cadence_plus") { if (dureeFlashMs <= 480) { dureeFlashMs += 20; } envoyerMenuStrobo(); }
    else if (commande == "/cadence_moins") { if (dureeFlashMs >= 40) { dureeFlashMs -= 20; } envoyerMenuStrobo(); }
    else if (commande == "/test_strobo") { executerFlashsStrobo(); }
  }
}

void setup() {
  pinMode(pinRelaisOption, OUTPUT); 
  pinMode(pinIN1, OUTPUT);
  pinMode(pinIN2, OUTPUT);
  pinMode(pinControlePuissance, OUTPUT);
  pinMode(pinRelaisStrobo, OUTPUT);
  
  digitalWrite(pinRelaisStrobo, LOW); 
  pinMode(pinSignalBarriere, INPUT_PULLUP); 
  
  randomSeed(analogRead(0)); 
  chargerParametres(); 

  analogWrite(pinControlePuissance, valeurPwmPuissance);
  digitalWrite(pinRelaisOption, relaisOptionEtat ? HIGH : LOW);

  Wire.begin(pinSDA, pinSCL);
  oled.begin(&Adafruit128x64, 0x3C); 
  oled.setFont(System5x7);
  
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  client.setInsecure();
  
  mettreAJourOled(); 
}

// =========================================================================
// VARIABLES ET BOUCLE DE BALAYAGE ASYNCHRONES
// =========================================================================
int fActuelle = 18000;              
unsigned long tempsDernierPalier = 0; 

void loop() {
  wl_status_t statutActuelWifi = WiFi.status();
  if (statutActuelWifi != ancienStatutWifi) {
    mettreAJourOled(); 
    if (statutActuelWifi == WL_CONNECTED) { bot.sendMessage(CHAT_ID, "🟢 Répulsif en ligne. Tapez /menu.", ""); }
    ancienStatutWifi = statutActuelWifi;
  }

  if (statutActuelWifi == WL_CONNECTED && (millis() - dernierScanTelegram > 2000)) {
    int numNewMessages = bot.getUpdates(bot.last_message_received + 1);
    while (numNewMessages) { gererNouveauxMessages(numNewMessages); numNewMessages = bot.getUpdates(bot.last_message_received + 1); }
    dernierScanTelegram = millis();
  }

  bool nouvelEtat = digitalRead(pinSignalBarriere);
  if (nouvelEtat == LOW && ancienEtatBarriere == HIGH && !systemeDesactiveManuel) {
    if (!enCoursEmission) {
      allumerUltrasons("DÉCLENCHEMENT BARRIÈRE");
    } else {
      ajouterRallongeAlerte();
    }
  }
  ancienEtatBarriere = nouvelEtat;

  if (enCoursEmission) {
    if (millis() - tempsDeclenchement > (dureeTotaleAlerte * 1000)) {
      eteindreUltrasons();
    } else {
      
      if (statutActuelWifi == WL_CONNECTED && (millis() - dernierMessageTempsRestant > 60000)) {
        long secondesRestantes = dureeTotaleAlerte - ((millis() - tempsDeclenchement) / 1000);
        if (secondesRestantes > 5) { 
          String msgSuivi = "⏳ **Alarme toujours en cours** — Reste : `" + String(secondesRestantes) + " secondes`.";
          String clavier = "[[{\"text\":\"🛑 Arrêter l'Alarme\",\"callback_data\":\"/stop_urgence\"}]]";
          bot.sendMessageWithInlineKeyboard(CHAT_ID, msgSuivi, "Markdown", clavier);
        }
        dernierMessageTempsRestant = millis();
      }
      
      if (enPhasePauseSalve) {
        analogWrite(pinIN1, 0); analogWrite(pinIN2, 0);
        if (millis() - tempsDebutPauseSalve > dureePauseAleatoire) { enPhasePauseSalve = false; }
      } 
      else {
        if (modeActuel == "SWEEP") {
          if (millis() - tempsDernierPalier > 15) {
            analogWriteFreq(fActuelle); 
            analogWrite(pinIN1, 512); 
            fActuelle += 200; 
            if (fActuelle > 22000) { fActuelle = 18000; }
            tempsDernierPalier = millis();
          }
        }
        if (random(0, 1000) == 5) {
          enPhasePauseSalve = true;
          tempsDebutPauseSalve = millis();
          dureePauseAleatoire = random(1000, 4000); 
        }
      }
    }
  }
}
