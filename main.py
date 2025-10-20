"""
Smart Gait - Sistema de monitoreo de actividad mediante sensores
Autor: jboulangger
"""
import os
import json
import logging
from datetime import datetime

import pyodbc
import paho.mqtt.client as mqtt
import joblib
import requests
from dotenv import load_dotenv
from azure.storage.blob import BlobServiceClient
import io

# Configuración básica de logging
logging.basicConfig(
    level=logging.INFO,
    format='%(asctime)s - %(name)s - %(levelname)s - %(message)s',
    handlers=[
        logging.FileHandler('smart_gait.log'),
        logging.StreamHandler()
    ]
)
logger = logging.getLogger('smart_gait')

logger.info("Iniciando aplicación Smart Gait")

load_dotenv()

MSSQL_USERNAME = os.getenv('MSSQL_USERNAME')
MSSQL_PASSWORD = os.getenv('MSSQL_PASSWORD')
MSSQL_SERVER = os.getenv('MSSQL_SERVER')
MSSQL_DATABASE = os.getenv('MSSQL_DATABASE')
MSSQL_PORT = os.getenv('MSSQL_PORT')
MSSQL_DRIVER = os.getenv('MSSQL_DRIVER')

BROKER = os.getenv('MOSQUITTO_BROKER')
PORT = int(os.getenv('MOSQUITTO_PORT'))
TOPIC = os.getenv('MOSQUITTO_TOPIC')
USERNAME = os.getenv('MOSQUITTO_USERNAME')
PASSWORD = os.getenv('MOSQUITTO_PASSWORD')

AZURE_STORAGE_CONNECTION_STRING = os.getenv('AZURE_CONNECTION_STRING')
AZURE_STORAGE_CONTAINER_NAME = os.getenv('AZURE_CONTAINER_NAME')

MSSQL_CONNECTION_STRING = (
    "DRIVER={" + MSSQL_DRIVER + "};"
    f"SERVER={MSSQL_SERVER};"
    f"DATABASE={MSSQL_DATABASE};"
    f"UID={MSSQL_USERNAME};"
    f"PWD={MSSQL_PASSWORD};"
    f"TrustServerCertificate=Yes;"
    f"Encrypt=No;"
)


URL_NTFY = "https://ntfy.sh/unfv_sensor_events_falls"

HEADERS =  {
    "Title": "Detección de anomalía",
    "Priority": "urgent",
    "Tags": "anomaly"
}

try:
    logger.info("Conectando a la base de datos...")
    connection = pyodbc.connect(MSSQL_CONNECTION_STRING) # pylint: disable=no-member
    cursor = connection.cursor() # pylint: disable=no-member
    logger.info("Conexión a la base de datos establecida")
except Exception as e:
    logger.error(f"Error al conectar a la base de datos: {e}")
    raise

# def load_model():
#     logger.info("Cargando modelo desde Azure Blob Storage...")
#     try:
#         blob_service_client = BlobServiceClient.from_connection_string(AZURE_STORAGE_CONNECTION_STRING)
#         blob_client = blob_service_client.get_blob_client(
#             container=AZURE_STORAGE_CONTAINER_NAME,
#             blob="modelo_rf.pkl"
#         )
#         logger.debug("Descargando modelo...")
#         model_bytes = io.BytesIO()
#         download_stream = blob_client.download_blob()
#         model_bytes.write(download_stream.readall())
#         model_bytes.seek(0)

#         logger.info("Modelo cargado exitosamente")
#         return joblib.load(model_bytes)
#     except Exception as e:
#         logger.error(f"Error al cargar el modelo: {e}")
#         raise

def load_model():
    return joblib.load("models/modelo_rf.pkl")
    
model = load_model()

def create_db():
    """ Create database and table """
    logger.info("Verificando existencia de la base de datos...")
    cursor.execute("SELECT name FROM sys.databases WHERE name = 'unfv'")
    if cursor.fetchone() is not None:
        logger.info("La base de datos ya existe")
        return

    try:
        logger.info("Creando base de datos...")
        conn_autocommit = pyodbc.connect(MSSQL_CONNECTION_STRING, autocommit=True)
        cursor_ac = conn_autocommit.cursor()
        cursor_ac.execute("CREATE DATABASE unfv")
        conn_autocommit.close()
        logger.info("Base de datos creada exitosamente")

        cursor.execute("USE unfv")
        connection.commit()
        logger.debug("Base de datos 'unfv' en uso")
    except Exception as e:
        logger.error(f"Error al crear la base de datos: {e}")
        raise

    try:
        logger.info("Creando tabla SENSOR_EVENTS...")
        cursor.execute("""
            CREATE TABLE [dbo].[SENSOR_EVENTS](
                [sensor_name] [varchar](255) NULL,
                [event_date_reg] [datetime] NULL,
                [avg_ax] [float] NULL,
                [avg_ay] [float] NULL,
                [avg_az] [float] NULL,
                [avg_gx] [float] NULL,
                [avg_gy] [float] NULL,
                [avg_gz] [float] NULL,
                [min_ax] [float] NULL,
                [min_ay] [float] NULL,
                [min_az] [float] NULL,
                [min_gx] [float] NULL,
                [min_gy] [float] NULL,
                [min_gz] [float] NULL,
                [max_ax] [float] NULL,
                [max_ay] [float] NULL,
                [max_az] [float] NULL,
                [max_gx] [float] NULL,
                [max_gy] [float] NULL,
                [max_gz] [float] NULL,
                [std_ax] [float] NULL,
                [std_ay] [float] NULL,
                [std_az] [float] NULL,
                [std_gx] [float] NULL,
                [std_gy] [float] NULL,
                [std_gz] [float] NULL,
                [value_predict] [varchar](255) NULL
            ) ON [PRIMARY]
        """)
        connection.commit()
        logger.info("Tabla SENSOR_EVENTS creada exitosamente")
    except Exception as e:
        logger.error(f"Error al crear la tabla: {e}")
        raise

create_db()

def on_connect(client, userdata, flags, reason_code, properties):
    """ The callback for when the client receives a CONNACK response from the server. """
    if reason_code == 0:
        logger.info(f"Conectado al broker MQTT. Suscrito a: {TOPIC}")
    else:
        logger.error(f"Error de conexión MQTT. Código: {reason_code}")
    client.subscribe(TOPIC)

def on_message(client, userdata, msg):
    """ The callback for when a PUBLISH message is received from the server. """
    logger.debug(f"Mensaje recibido - Tópico: {msg.topic}, Payload: {msg.payload}")
    try:
        data = json.loads(msg.payload)
        logger.info(f"Datos recibidos del sensor: {data.get('sensor_name', 'desconocido')}")
    except json.JSONDecodeError:
        logger.error("Error al decodificar el mensaje JSON")
        return

    default = None

    sensor_name = data.get('sensor_name', default)
    event_date_reg = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
    avg_ax = data.get('avg_ax', default)
    avg_ay = data.get('avg_ay', default)
    avg_az = data.get('avg_az', default)
    avg_gx = data.get('avg_gx', default)
    avg_gy = data.get('avg_gy', default)
    avg_gz = data.get('avg_gz', default)
    min_ax = data.get('min_ax', default)
    min_ay = data.get('min_ay', default)
    min_az = data.get('min_az', default)
    min_gx = data.get('min_gx', default)
    min_gy = data.get('min_gy', default)
    min_gz = data.get('min_gz', default)
    max_ax = data.get('max_ax', default)
    max_ay = data.get('max_ay', default)
    max_az = data.get('max_az', default)
    max_gx = data.get('max_gx', default)
    max_gy = data.get('max_gy', default)
    max_gz = data.get('max_gz', default)
    std_ax = data.get('std_ax', default)
    std_ay = data.get('std_ay', default)
    std_az = data.get('std_az', default)
    std_gx = data.get('std_gx', default)
    std_gy = data.get('std_gy', default)
    std_gz = data.get('std_gz', default)
    value_predict =  default

    try:
        X = [[
            avg_ax, avg_ay, avg_az,
            avg_gx, avg_gy, avg_gz,
            min_ax, min_ay, min_az,
            min_gx, min_gy, min_gz,
            max_ax, max_ay, max_az,
            max_gx, max_gy, max_gz,
            std_ax, std_ay, std_az,
            std_gx, std_gy, std_gz
        ]]
        logger.debug("Realizando predicción...")
        value_predict = str(model.predict(X)[0])
        logger.debug(f"Resultado de la predicción: {value_predict}")
    except Exception as e:
        logger.error(f"Error en la predicción: {e}")
        raise

    if value_predict == "1":
        value_predict = "Parado"
        logger.info("Estado detectado: Parado")

    if value_predict == "2":
        value_predict = "Caminando"
        logger.info("Estado detectado: Caminando")

    if value_predict == "3":
        value_predict = "Anomalía"
        logger.warning("¡Anomalía detectada!")
        #data = "Anomalía detectada: Posible caída de sensor_01. Por favor, contactar de emergencia."
        #try:
        #    response = requests.post(URL_NTFY, json=data, headers=HEADERS)
        #    response.raise_for_status()
        #    logger.info("Notificación de anomalía enviada")
        #except Exception as e:
        #    logger.error(f"Error al enviar notificación: {e}")

    if value_predict == "5":
        value_predict = "Tumbado"
        logger.info("Estado detectado: Tumbado")
    try:
        query = """
            INSERT INTO [UNFV].[dbo].[SENSOR_EVENTS]( sensor_name, event_date_reg, avg_ax, avg_ay, avg_az, avg_gx, avg_gy, avg_gz, min_ax, min_ay, min_az, min_gx, min_gy, min_gz, max_ax, max_ay, max_az, max_gx, max_gy, max_gz, std_ax, std_ay, std_az, std_gx, std_gy, std_gz, value_predict)
            VALUES( ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
        """
        cursor.execute(query, (
            sensor_name, event_date_reg, avg_ax, avg_ay, avg_az,
            avg_gx, avg_gy, avg_gz, min_ax, min_ay, min_az,
            min_gx, min_gy, min_gz, max_ax, max_ay, max_az,
            max_gx, max_gy, max_gz, std_ax, std_ay, std_az,
            std_gx, std_gy, std_gz, value_predict
        ))
        cursor.commit()
        connection.commit()
        logger.debug("Datos guardados en la base de datos")
    except Exception as e:
        logger.error(f"Error al guardar en la base de datos: {e}")
        connection.rollback()
        raise

logger.info(f"Iniciando cliente MQTT en {BROKER}:{PORT}")
mqttc = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
mqttc.on_connect = on_connect
mqttc.on_message = on_message

mqttc.connect(BROKER, PORT)
mqttc.username_pw_set(USERNAME, PASSWORD)
logger.info("Iniciando bucle de eventos MQTT...")
mqttc.loop_forever()
