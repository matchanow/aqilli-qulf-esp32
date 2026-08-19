FROM python:3.12-slim

WORKDIR /app

# Kutubxonalar alohida qatlamda - kod o'zgarganda qayta o'rnatilmaydi
COPY requirements.txt .
RUN pip install --no-cache-dir -r requirements.txt

COPY . .

# Baza konteyner ichida emas, tashqi hajmda (volume) turishi kerak -
# aks holda har yangilanishda kartalar yo'qoladi.
VOLUME ["/app/data"]

EXPOSE 8000
CMD ["uvicorn", "app:app", "--host", "0.0.0.0", "--port", "8000"]
