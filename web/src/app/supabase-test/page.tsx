import { connection } from 'next/server'
import { redirect } from 'next/navigation'

import { createClient } from '@/lib/supabase/server'

export default async function SupabaseTestPage() {
  await connection()
  const supabase = await createClient()
  const { data: identity } = await supabase.auth.getClaims()
  if (!identity?.claims?.sub) redirect('/login')

  let data: unknown = null
  let errorMessage: string | null = null

  try {
    const result = await supabase
      .from('positions')
      .select(
        'id, household_id, device_uid, message_id, latitude, longitude, battery, source, recorded_at'
      )
      .order('recorded_at', { ascending: false })
      .limit(1)

    if (result.error) {
      throw result.error
    }

    data = result.data
  } catch (error) {
    errorMessage = error instanceof Error ? error.message : 'Unknown Supabase error'
  }

  return (
    <main style={{ padding: '40px' }}>
      <h1>Supabase Test</h1>
      <pre>
        {errorMessage
          ? `Unable to read positions: ${errorMessage}`
          : JSON.stringify(data, null, 2)}
      </pre>
    </main>
  )
}
