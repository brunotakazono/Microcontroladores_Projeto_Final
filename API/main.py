from fastapi import FastAPI, HTTPException, Depends, Request, Form
from fastapi.responses import HTMLResponse, RedirectResponse
from sqlalchemy.orm import Session
from sqlalchemy import and_
from datetime import datetime
from models import Client, Timestamp, Invoice, Base
from database import engine, SessionLocal
from fastapi.templating import Jinja2Templates
from fastapi.responses import JSONResponse
import pytz



app = FastAPI()

templates = Jinja2Templates(directory="templates")

def get_db():
    db = SessionLocal()
    try:
        yield db
    finally:
        db.close()

Base.metadata.create_all(bind=engine)

@app.post("/timestamps")
def register_timestamp(uid: str, db: Session = Depends(get_db)):
    db_client = db.query(Client).filter(Client.uid == uid).first()
    if not db_client:
        raise HTTPException(status_code=404, detail="UID não registrado")
    
    existing_timestamp = db.query(Timestamp).filter(
        and_(
            Timestamp.uid == uid,
            Timestamp.exit_time == None  
        )
    ).first()

    if existing_timestamp:
        existing_timestamp.exit_time = datetime.now()
        db.commit()
        return {"message": "exit_registered"}
    else:
        new_timestamp = Timestamp(uid=uid, entry_time=datetime.now())
        db.add(new_timestamp)
        db.commit()
        return {"message": "entry_registered"}

@app.get("/register", response_class=HTMLResponse)
def register_client_form(request: Request):
    return templates.TemplateResponse("register.html", {"request": request})

@app.post("/register/")
def register_client(uid: str = Form(...), name: str = Form(...), db: Session = Depends(get_db)):
    db_client = db.query(Client).filter(Client.uid == uid).first()
    if db_client:
        raise HTTPException(status_code=400, detail="UID already registered")
    new_client = Client(uid=uid, name=name)
    db.add(new_client)
    db.commit()
    return RedirectResponse(url="/", status_code=302)

@app.get("/invoice", response_class=HTMLResponse)
async def get_invoice_form(request: Request):
    return templates.TemplateResponse("invoice.html", {"request": request})

@app.post("/invoice/")
async def generate_invoice(
    request: Request,
    name: str = Form(...),
    start_date: str = Form(...),
    end_date: str = Form(...),
    rate_per_hour: float = Form(...),
    db: Session = Depends(get_db)
):
    db_client = db.query(Client).filter(Client.name == name).first()
    if not db_client:
        raise HTTPException(status_code=404, detail="Client not found")
    
    uid = db_client.uid

    try:
        start_date = datetime.strptime(start_date, '%Y-%m-%d').replace(tzinfo=pytz.UTC)
        end_date = datetime.strptime(end_date, '%Y-%m-%d').replace(tzinfo=pytz.UTC)
    except ValueError:
        raise HTTPException(status_code=400, detail="Invalid date format")

    timestamps = db.query(Timestamp).filter(
        Timestamp.uid == uid,
        Timestamp.exit_time >= start_date,
        Timestamp.entry_time <= end_date
    ).all()
    
    total_hours = 0
    for ts in timestamps:
        if ts.entry_time.tzinfo is None:
            ts.entry_time = pytz.UTC.localize(ts.entry_time)
        if ts.exit_time.tzinfo is None:
            ts.exit_time = pytz.UTC.localize(ts.exit_time)

        overlap_start = max(ts.entry_time, start_date)
        overlap_end = min(ts.exit_time, end_date)

        if overlap_start < overlap_end: 
            duration = (overlap_end - overlap_start).total_seconds() / 3600
            total_hours += duration
    
    total_amount = total_hours * rate_per_hour

    invoice = Invoice(
        name=name,
        start_date=start_date,
        end_date=end_date,
        total_hours=total_hours,
        total_amount=total_amount
    )
    db.add(invoice)
    db.commit()
    
    return templates.TemplateResponse("invoice_result.html", {
        "request": request,
        "name": name,
        "total_hours": total_hours,
        "total_amount": total_amount
    })

@app.get("/check_uid/{uid}")
def check_uid(uid: str, db: Session = Depends(get_db)):
    db_client = db.query(Client).filter(Client.uid == uid).first()
    if db_client:
        return JSONResponse(content={"message": "UID registrado", "name": db_client.name})
    else:
        return JSONResponse(content={"message": "UID não registrado"})

@app.get("/", response_class=HTMLResponse)
def read_root(request: Request):
    return templates.TemplateResponse("index.html", {"request": request})
