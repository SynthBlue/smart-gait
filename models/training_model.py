import os

import pandas as pd
from dotenv import load_dotenv

load_dotenv()

load_dotenv()
AZURE_CONNECTION_STRING=os.getenv('AZURE_CONNECTION_STRING')
AZURE_CONTAINER_NAME=os.getenv('AZURE_CONTAINER_NAME')

df = pd.read_csv("dataset/data_raw.csv")

if df.empty:
    df = pd.read_excel("dataset/data_raw.xlsx")
    df.to_csv("dataset/data_raw.csv", index=False)


from sklearn.model_selection import train_test_split
from sklearn.ensemble import RandomForestClassifier

X = df.drop(columns=["value_predict", "sensor_name", "event_date_reg"])
y = df["value_predict"]

X_train, X_test, y_train, y_test = train_test_split(
    X, y, test_size=0.2, random_state=42, stratify=y
)

clf = RandomForestClassifier(
    n_estimators=200, 
    max_depth=None, 
    random_state=42,
    n_jobs=-1
)

clf.fit(X_train, y_train)

from sklearn.metrics import classification_report, confusion_matrix

y_pred = clf.predict(X_test)

import joblib

# Entrenas tu modelo
clf = RandomForestClassifier(
    n_estimators=200, 
    max_depth=None, 
    random_state=42,
    n_jobs=-1
)

clf.fit(X_train, y_train)

joblib.dump(clf, "models/modelo_rf.pkl")