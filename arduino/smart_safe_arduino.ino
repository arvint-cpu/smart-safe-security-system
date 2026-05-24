// =========================
// ARDUINO CODE (TCS3200 + SERVO + PIEZOS) — RGBW + actions
// Upload as: arduino_tcs3200_servo_rgbw_actions.ino
//
// Commands from Pi (Serial 9600):
//   COLOR?  -> replies: COLOR:RED / COLOR:GREEN / COLOR:BLUE / COLOR:WHITE / COLOR:UNKNOWN
//   LOCK    -> servo 0°,   OK:LOCK
//   UNLOCK  -> servo 180°, OK:UNLOCK
//   BEEP    -> short chirps on piezos, OK:BEEP
//   SIREN   -> "haywire" pattern on piezos, OK:SIREN
//
// Behavior:
// - On boot: servo LOCK (0°)
// - COLOR? : waits settle, averages samples, classifies RGBW, returns result
//
// Pin map (your FINAL):
//   TCS3200: S0 D2, S1 D3, S2 D4, S3 D5, OUT D11, VCC 5V, GND GND
//   Servo: signal D9 (power external 5V), GND shared
//   Piezo: D6, D7, D8 (negatives to GND)
// =========================


#include <Servo.h>


// ---- Pins ----
const int S0_PIN  = 2;
const int S1_PIN  = 3;
const int S2_PIN  = 4;
const int S3_PIN  = 5;
const int OUT_PIN = 11;


const int SERVO_PIN = 9;


const int PIEZO1 = 6;
const int PIEZO2 = 7;
const int PIEZO3 = 8;


// ---- Servo angles ----
const int LOCK_ANGLE   = 0;
const int UNLOCK_ANGLE = 180;


// ---- Color scan settings ----
const int SETTLE_SECONDS = 3;                 // Pi already waits, but keep as safety
const int VOTE_SAMPLES = 7;                   // majority vote (odd is nice)
const unsigned long PULSE_TIMEOUT = 100000UL; // 100ms timeout


Servo lockServo;


// Helpers
String readLineNonBlocking() {
 if (!Serial.available()) return "";
 String s = Serial.readStringUntil('\n');
 s.trim();
 s.toUpperCase();
 return s;
}


void setScaling20() {
 // 20% scaling: S0=HIGH, S1=LOW
 digitalWrite(S0_PIN, HIGH);
 digitalWrite(S1_PIN, LOW);
}


void setFilterRed()   { digitalWrite(S2_PIN, LOW);  digitalWrite(S3_PIN, LOW);  }
void setFilterGreen() { digitalWrite(S2_PIN, HIGH); digitalWrite(S3_PIN, HIGH); }
void setFilterBlue()  { digitalWrite(S2_PIN, LOW);  digitalWrite(S3_PIN, HIGH); }
void setFilterClear() { digitalWrite(S2_PIN, HIGH); digitalWrite(S3_PIN, LOW);  } // "clear" (no filter)


unsigned long readPulseAvg(int samples) {
 unsigned long total = 0;
 int got = 0;
 for (int i = 0; i < samples; i++) {
   unsigned long t = pulseIn(OUT_PIN, LOW, PULSE_TIMEOUT);
   if (t == 0) continue;
   total += t;
   got++;
   delay(3);
 }
 if (got == 0) return 0;
 return total / (unsigned long)got;
}


void servoLock()   { lockServo.write(LOCK_ANGLE); }
void servoUnlock() { lockServo.write(UNLOCK_ANGLE); }


// Piezo patterns
void beepOnce(int pin, int freq, int ms) {
 tone(pin, freq, ms);
 delay(ms + 5);
 noTone(pin);
}


void beepAllShort() {
 // quick triple chirp across pins
 for (int i = 0; i < 2; i++) {
   beepOnce(PIEZO1, 1800, 80);
   beepOnce(PIEZO2, 1400, 80);
   beepOnce(PIEZO3, 2200, 80);
   delay(60);
 }
}


void sirenHaywire(unsigned long totalMs = 2200) {
 unsigned long start = millis();
 while (millis() - start < totalMs) {
   int f1 = random(700, 2500);
   int f2 = random(700, 2500);
   int f3 = random(700, 2500);
   int d  = random(40, 120);


   tone(PIEZO1, f1);
   tone(PIEZO2, f2);
   tone(PIEZO3, f3);
   delay(d);


   noTone(PIEZO1);
   noTone(PIEZO2);
   noTone(PIEZO3);
   delay(random(10, 60));
 }
}


// RGBW Classification
// Notes:
// - TCS3200 returns frequency; using pulse width (LOW) => lower width = higher intensity
// - We compare relative strengths rather than absolute.
String classifyOnceRGBW() {
 // read channels
 setFilterRed();   delay(50); unsigned long r = readPulseAvg(10);
 setFilterGreen(); delay(50); unsigned long g = readPulseAvg(10);
 setFilterBlue();  delay(50); unsigned long b = readPulseAvg(10);
 setFilterClear(); delay(50); unsigned long c = readPulseAvg(10);


 if (r == 0 || g == 0 || b == 0 || c == 0) return "UNKNOWN";


 // If nearly identical (no strong dominance) it could be white/gray/unknown.
 unsigned long maxRGB = max(r, max(g, b));
 unsigned long minRGB = min(r, min(g, b));
 unsigned long spread = maxRGB - minRGB;


 // White heuristic:
 // - clear channel indicates strong light (small pulse width)
 // - RGB channels are close to each other (small spread)
 // Thresholds are tuned for typical classroom sensors; adjust if needed.
 const unsigned long WHITE_SPREAD_MAX = 22;   // smaller = stricter "white"
 const float CLEAR_STRONG_RATIO = 0.88;       // c must be <= 0.88 * avgRGB


 float avgRGB = (r + g + b) / 3.0;
 bool rgbClose = (spread <= WHITE_SPREAD_MAX);


 // "Strong" means smaller pulse width
 bool clearStrong = ((float)c <= avgRGB * CLEAR_STRONG_RATIO);


 if (rgbClose && clearStrong) {
   return "WHITE";
 }


 // Dominance ratios (smaller pulse => stronger)
 // A color is "dominant" if its pulse width is significantly smaller than the other two.
 const float DOM = 0.78; // dominant must be < 0.78 * other


 bool isRed   = ((float)r < (float)g * DOM) && ((float)r < (float)b * DOM);
 bool isGreen = ((float)g < (float)r * DOM) && ((float)g < (float)b * DOM);
 bool isBlue  = ((float)b < (float)r * DOM) && ((float)b < (float)g * DOM);


 // If multiple dominants (rare noise), call unknown
 int domCount = (isRed ? 1 : 0) + (isGreen ? 1 : 0) + (isBlue ? 1 : 0);
 if (domCount != 1) return "UNKNOWN";


 if (isRed) return "RED";
 if (isGreen) return "GREEN";
 return "BLUE";
}


String scanColorVoted() {
 // settle (extra safety)
 for (int i = 0; i < SETTLE_SECONDS; i++) delay(1000);


 int redC = 0, greenC = 0, blueC = 0, whiteC = 0, unkC = 0;


 for (int i = 0; i < VOTE_SAMPLES; i++) {
   String c = classifyOnceRGBW();
   if      (c == "RED")   redC++;
   else if (c == "GREEN") greenC++;
   else if (c == "BLUE")  blueC++;
   else if (c == "WHITE") whiteC++;
   else                   unkC++;
   delay(70);
 }


 // Winner-take-most; if tie -> UNKNOWN
 int best = redC;
 String bestC = "RED";


 if (greenC > best) { best = greenC; bestC = "GREEN"; }
 if (blueC  > best) { best = blueC;  bestC = "BLUE";  }
 if (whiteC > best) { best = whiteC; bestC = "WHITE"; }


 // Check tie
 int ties = 0;
 if (redC == best) ties++;
 if (greenC == best) ties++;
 if (blueC == best) ties++;
 if (whiteC == best) ties++;


 if (best == 0) return "UNKNOWN";
 if (ties > 1)  return "UNKNOWN";
 return bestC;
}


// Setup / Loop
void setup() {
 Serial.begin(9600);
 delay(250);


 pinMode(S0_PIN, OUTPUT);
 pinMode(S1_PIN, OUTPUT);
 pinMode(S2_PIN, OUTPUT);
 pinMode(S3_PIN, OUTPUT);
 pinMode(OUT_PIN, INPUT);


 pinMode(PIEZO1, OUTPUT);
 pinMode(PIEZO2, OUTPUT);
 pinMode(PIEZO3, OUTPUT);


 setScaling20();


 lockServo.attach(SERVO_PIN);
 servoLock();
 delay(250);


 randomSeed(analogRead(A0));


 Serial.println("BOOT:READY");
 Serial.println("READY:CMD=COLOR?/LOCK/UNLOCK/BEEP/SIREN");
}


void loop() {
 String cmd = readLineNonBlocking();
 if (cmd.length() == 0) return;


 if (cmd == "LOCK") {
   servoLock();
   Serial.println("OK:LOCK");
 }
 else if (cmd == "UNLOCK") {
   servoUnlock();
   Serial.println("OK:UNLOCK");
 }
 else if (cmd == "BEEP") {
   beepAllShort();
   Serial.println("OK:BEEP");
 }
 else if (cmd == "SIREN") {
   sirenHaywire(2200);
   Serial.println("OK:SIREN");
 }
 else if (cmd == "COLOR?") {
   String result = scanColorVoted();
   Serial.print("COLOR:");
   Serial.println(result);
 }
 else {
   Serial.println("ERR:UNKNOWN_CMD");
 }
}





