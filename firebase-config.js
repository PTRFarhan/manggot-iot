// Config Firebase project manggot-iot (aman di-upload ke GitHub, keamanan diatur oleh Database Rules)
export const firebaseConfig = {
  apiKey: "AIzaSyAzGu70xnEhxBrn97APbRS3od-qsi0dNig",
  authDomain: "manggot-iot.firebaseapp.com",
  databaseURL: "https://manggot-iot-default-rtdb.asia-southeast1.firebasedatabase.app",
  projectId: "manggot-iot",
  storageBucket: "manggot-iot.firebasestorage.app",
  messagingSenderId: "1002200519209",
  appId: "1:1002200519209:web:bd5317ce51b007c33ac238"
};

// Harus sama dengan DEVICE_ID di secrets.h pada ESP32
export const DEVICE_ID = "manggot01";

// Alamat aplikasi di GitHub Pages (dipakai aplikasi Android untuk update firmware & link APK)
export const SITE_URL = "https://ptrfarhan.github.io/manggot-iot/";
