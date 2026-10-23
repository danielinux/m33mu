import React from 'react';
import { createRoot } from 'react-dom/client';
import { Workbench } from './workbench';
import './style.css';

createRoot(document.getElementById('root')!).render(
  <React.StrictMode><Workbench /></React.StrictMode>
);
