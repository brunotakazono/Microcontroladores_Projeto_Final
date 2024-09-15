from sqlalchemy import Column, Integer, String, Float, DateTime
from sqlalchemy.ext.declarative import declarative_base
from sqlalchemy.sql import func
from datetime import datetime
from sqlalchemy.orm import relationship
import pytz

Base = declarative_base()

class Client(Base):
    __tablename__ = "clients"
    
    id = Column(Integer, primary_key=True, index=True)
    uid = Column(String, unique=True, index=True)
    name = Column(String)

class Timestamp(Base):
    __tablename__ = "timestamps"
    
    id = Column(Integer, primary_key=True, index=True)
    uid = Column(String, index=True)
    entry_time = Column(DateTime, default=func.now())
    exit_time = Column(DateTime, nullable=True)


class Invoice(Base):
    __tablename__ = "invoices"
    
    id = Column(Integer, primary_key=True, index=True)
    name = Column(String, index=True)
    start_date = Column(DateTime)
    end_date = Column(DateTime)
    total_hours = Column(Float)
    total_amount = Column(Float)
    created_at = Column(DateTime, default=lambda: datetime.now(pytz.UTC))